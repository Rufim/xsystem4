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
#include <signal.h>

#include "system4.h"
#include "system4/cg.h"
#include "system4/hashtable.h"
#include "system4/instructions.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "apeg_movie.h"
#include "asset_manager.h"
#include "audio.h"
#include "vm.h"
#include "vm/page.h"
#include "xsystem4.h"
#include "sact.h"
#include "sprite.h"
#include "parts.h"
#include "parts_internal.h"
#include "reign.h"

struct parts_list parts_list = TAILQ_HEAD_INITIALIZER(parts_list);
static struct parts_list dirty_list = TAILQ_HEAD_INITIALIZER(dirty_list);
static struct hash_table *parts_table = NULL;
static Point root_pos = { 0, 0 };

/*
 * Тип компонента, назначенный номеру ДО создания парта. Игровые обёртки
 * (parts::detail::C*Parts@0) зовут SetComponentType сразу в конструкторе, когда
 * парта ещё нет, — материализовать парт в этот момент нельзя (это создание из
 * ЗАЯВКИ, реального парта у игры ещё нет; кто и когда создаёт по-настоящему —
 * SetPartsCG/SetText/…). Тип запоминаем здесь и применяем при первом реальном
 * создании парта, чтобы не потерять флаги вроде is_button (Ixseal конструирует
 * кнопки в рантайме тем же путём).
 */
static struct hash_table *pending_ctype_table = NULL;

struct pending_ctype { int type; int state; };

static void pending_ctype_set(int parts_no, int type, int state)
{
	/*
	 * Невалидный номер — мина замедленного действия: игра зовёт
	 * SetComponentType(GetPartsNumber(...), 17), и когда парт не найден (-1),
	 * заявка «парт -1 — контейнер» оседала в таблице. Дальше любой обход
	 * детей, наткнувшись на дырку перечисления (GetChild → -1), спрашивал
	 * GetComponentType(-1) == 17 и обходил «детей -1» — все КОРНЕВЫЕ парты —
	 * заново, до переполнения стека вызовов (после загрузки сейва).
	 */
	if (parts_no < 0) {
		WARNING("SetComponentType(%d, %d): невалидный номер парта, заявка отброшена",
		        parts_no, type);
		return;
	}
	if (!pending_ctype_table)
		pending_ctype_table = ht_create(64);
	struct ht_slot *slot = ht_put_int(pending_ctype_table, parts_no, NULL);
	struct pending_ctype *p = slot->value;
	if (!p) {
		p = xmalloc(sizeof(*p));
		slot->value = p;
	}
	p->type = type;
	p->state = state;
}

static struct pending_ctype *pending_ctype_get(int parts_no)
{
	if (!pending_ctype_table)
		return NULL;
	return ht_get_int(pending_ctype_table, parts_no, NULL);
}

static void pending_ctype_clear(int parts_no)
{
	if (!pending_ctype_table)
		return;
	struct ht_slot *slot = ht_put_int(pending_ctype_table, parts_no, NULL);
	if (slot->value) {
		free(slot->value);
		slot->value = NULL;
	}
}

static bool shifted_component_types(void);
bool parts_exists(int parts_no);

/*
 * ★ОТВЕРГНУТО ЗАМЕРОМ, не возвращать: «НАДГРОБИЯ» — запоминать при освобождении
 * последнюю видимость парта и возвращать её, если номер воскрешает обращение без
 * заявки SetComponentType (то есть мёртвая тикающая обёртка).
 * Не работает ПРИНЦИПИАЛЬНО: номер переиспользуется РАЗНЫМИ владельцами, и
 * надгробие фиксирует состояние ПОСЛЕДНЕГО из них, а не того, кто тикает. В
 * замере плёночный шум титула воскрешал номер 1000001050, надгробие которого
 * оставил юнит партии `actionselect::CPartyUnit` с show=1 — прямоугольник
 * остался видимым. По номеру отличить «чей» зомби движок не может.
 */

/*
 * ★ОТВЕРГНУТО ЗАМЕРОМ, не возвращать: «у v14 парт создаётся только по заявке
 * SetComponentType из конструктора обёртки, поэтому SetPartsCG на
 * несуществующем парте — тик мёртвой обёртки, создавать нельзя».
 * Результат: ЧЁРНЫЙ ЭКРАН с первого кадра (не отрисовалось даже лого Alice) —
 * SetPartsCG у v14 тоже штатный путь создания части.
 */

struct parts_controller_stack ctrl_stack;
bool parts_multi_controller;

static void ctrl_stack_init(void);

#define PARTS_PARAMS_INITIALIZER (struct parts_params) { \
	.z = 1, \
	.pos = { 0, 0 }, \
	.show = true, \
	.alpha = 255, \
	.scale = { 1.0f, 1.0f }, \
	.rotation = { 0.0f, 0.0f, 0.0f }, \
	.add_color = { 0, 0, 0, 0 }, \
	.multiply_color = { 255, 255, 255, 255 } \
}

static void parts_init(struct parts *parts)
{
	parts->sp.z = 1;
	parts->sp.has_pixel = true;
	parts->sp.has_alpha = true;
	parts->sp.render = parts_sprite_render;
	parts->sp.to_json = parts_sprite_to_json;
	parts->local = PARTS_PARAMS_INITIALIZER;
	parts->global = PARTS_PARAMS_INITIALIZER;
	// ★ОТВЕРГНУТО ЗАМЕРОМ (дважды), не возвращать: «создавать парт скрытым» —
	// ни для всех партов v14, ни только для тех, кого создают без заявки
	// SetComponentType. В обоих случаях зомби-плёнка гаснет, но ВМЕСТЕ С НЕЙ
	// пропадает фон ADV-сцены с дождём: явного SetShow(1) он не получает.
	parts->delegate_index = -1;
	parts->event_unique_id = -1;
	parts->want_save = true;
	// Игра зовёт SetWantSaveBackScene только с enable=0 (пять мест в байткоде) ⇒ дефолт «да».
	parts->want_save_back_scene = true;
	parts->enable_input_process = true;
	parts->checkbox_enabled = true;
	parts->wheelable = true;
	parts->on_cursor_sound = -1;
	parts->on_click_sound = -1;
	parts->origin_mode = 1;
	parts->pending_parent = -1;
	parts->linked_to = -1;
	parts->linked_from = -1;
	parts->on_cursor_show_link = -1;
	TAILQ_INIT(&parts->children);
	TAILQ_INIT(&parts->motion);
}

/*
 * `XSYS4_PART_WATCH=<номер>[,<номер>…]` — следить за конкретными партами: создание (и на
 * КАКОМ СЛОЕ), освобождение, show. Нужен там, где общий трейс тонет в шуме: у Haha Ranman
 * на ADV-сцене остаются чужие парты, а сцена создаёт их тысячами (один только дождь —
 * сотня 1x32), так что «печатать всё» бесполезно.
 */
/*
 * ДОБАВЛЯЕМЫЙ НА ХОДУ СПИСОК НАБЛЮДЕНИЯ. `XSYS4_PART_WATCH` задаёт номера заранее, но
 * номера частей МЕНЯЮТСЯ между прогонами, поэтому для «поймать вот эту часть по имени»
 * он бесполезен. Реестр активностей знает имена (`GetActivityPartsNumber`), и по
 * `XSYS4_WATCH_ACT=<подстрока имени активности>` найденные там части попадают сюда.
 */
#define PARTS_WATCH_DYN_MAX 64
static int parts_watch_dyn[PARTS_WATCH_DYN_MAX];
static int parts_watch_dyn_nr = 0;

void parts_watch_add(int parts_no)
{
	for (int i = 0; i < parts_watch_dyn_nr; i++)
		if (parts_watch_dyn[i] == parts_no)
			return;
	if (parts_watch_dyn_nr >= PARTS_WATCH_DYN_MAX)
		return;
	parts_watch_dyn[parts_watch_dyn_nr++] = parts_no;
	NOTICE("PARTWATCH часть %d взята под наблюдение", parts_no);
}

bool parts_watched(int parts_no)
{
	// Парт, пойманный по КАРТИНКЕ (XSYS4_CG_WATCH), тоже считается наблюдаемым —
	// иначе на каждом месте пришлось бы проверять два условия подряд.
	if (parts_no >= 0 && parts_no == parts_cg_watch_part())
		return true;
	for (int i = 0; i < parts_watch_dyn_nr; i++)
		if (parts_watch_dyn[i] == parts_no)
			return true;
	static const char *watch = (const char *)1;
	if (watch == (const char *)1)
		watch = getenv("XSYS4_PART_WATCH");
	if (!watch || !*watch)
		return false;
	for (const char *p = watch; *p; ) {
		long v = strtol(p, (char **)&p, 0);
		if (v == parts_no)
			return true;
		while (*p && *p != ',')
			p++;
		if (*p == ',')
			p++;
	}
	return false;
}

static struct parts *parts_alloc(void)
{
	struct parts *parts = xcalloc(1, sizeof(struct parts));
	parts_init(parts);
	return parts;
}

void parts_component_dirty(struct parts *parts)
{
	if (parts->dirty)
		return;
	parts->dirty = true;
	TAILQ_INSERT_TAIL(&dirty_list, parts, dirty_list_entry);
}

static void dirty_list_remove(struct parts *parts)
{
	if (parts->dirty)
		TAILQ_REMOVE(&dirty_list, parts, dirty_list_entry);
}

static int ctrl_stack_pos(int id);

static int parts_get_sprite_z(struct parts *parts)
{
	if (!parts_multi_controller)
		return parts->global.z;
	// ★Порядок слоёв задаёт ПОЗИЦИЯ В СТЕКЕ, а не id: id — просто выданный номер,
	// и после сноса слоя из середины он уже не совпадает с глубиной (наш id —
	// наименьший свободный, так что заново созданный слой может получить МАЛЫЙ
	// номер и оказаться на вершине стека). Пока ключом был id, свежий экран мог
	// уехать ПОД старый — у Haha Ranman так титул (id 3, позиция 2) сортировался
	// выше сцены, добавленной после него (id 2, позиция 3).
	// The system overlay controller sorts above any in-stack controller.
	if (parts->controller_no == PARTS_CONTROLLER_SYSTEM_OVERLAY)
		return PARTS_CONTROLLER_STACK_MAX;
	int pos = ctrl_stack_pos(parts->controller_no);
	/*
	 * ★ЧАСТЬ БЕЗ СЛОЯ — СОСТОЯНИЕ ОЖИДАНИЯ. Правильного ключа сортировки для неё
	 * не существует: 0 — это ключ самого нижнего слоя, и часть не «уходит под
	 * всё», а всплывает НАД ним (вторичный ключ — её собственный z, у остатков боя
	 * это были сотни тысяч против единиц у ADV-слоя); отрицательный ключ
	 * спрайт-сцена не принимает вовсе, и часть пропадает совсем.
	 *
	 * Вопрос снят тем, что такие части НЕ РИСУЮТСЯ (`parts_hidden_by_layer`):
	 * они принадлежат живой активности и ждут, пока игра к ним обратится и
	 * `parts_adopt_to_active_layer` вернёт их на текущий слой. Ключ здесь нужен
	 * лишь для того, чтобы список оставался упорядоченным, — берём нижний.
	 */
	return pos >= 0 ? pos : 0;
}

static int parts_get_sprite_z2(struct parts *parts)
{
	if (!parts_multi_controller)
		return 0;
	return parts->global.z;
}

static void parts_list_insert(struct parts *parts)
{
	int z = parts_get_sprite_z(parts);
	int z2 = parts_get_sprite_z2(parts);
	parts->sp.z = z;
	parts->sp.z2 = z2;
	struct parts *p;
	PARTS_LIST_FOREACH(p) {
		int pz = parts_get_sprite_z(p);
		int pz2 = parts_get_sprite_z2(p);
		if (pz > z || (pz == z && pz2 > z2)) {
			TAILQ_INSERT_BEFORE(p, parts, parts_list_entry);
			goto done;
		}
	}
	TAILQ_INSERT_TAIL(&parts_list, parts, parts_list_entry);
done:
	parts_engine_dirty();
	scene_register_sprite(&parts->sp);
}

static void parts_list_remove(struct parts *parts)
{
	TAILQ_REMOVE(&parts_list, parts, parts_list_entry);
	scene_unregister_sprite(&parts->sp);
}

void parts_list_resort(struct parts *parts)
{
	// TODO: this could be optimized
	parts_list_remove(parts);
	parts_list_insert(parts);
}

struct parts *parts_try_get(int parts_no)
{
	struct ht_slot *slot = ht_put_int(parts_table, parts_no, NULL);
	if (slot->value)
		return slot->value;
	return NULL;
}

struct parts *parts_get(int parts_no)
{
	struct ht_slot *slot = ht_put_int(parts_table, parts_no, NULL);
	if (slot->value)
		return slot->value;

	struct parts *parts = parts_alloc();
	parts->no = parts_no;
	parts->controller_no = ctrl_stack.active;
	if (parts_watched(parts_no)) {
		NOTICE("PARTWATCH создан парт %d на слое %d (стек-глубина %d) — стек вызовов игры:",
		       parts_no, parts->controller_no, ctrl_stack.nr_controllers);
		vm_stack_trace();
	}
	slot->value = parts;
	parts_list_insert(parts);
	// Тип компонента, назначенный номеру до создания, применяется при
	// материализации (см. pending_ctype_table).
	struct pending_ctype *pc = pending_ctype_get(parts_no);
	if (pc) {
		int t = pc->type, s = pc->state;
		pending_ctype_clear(parts_no);
		PE_SetComponentType(parts_no, t, s);
	}
	return parts;
}

bool parts_exists(int parts_no)
{
	return !!ht_get_int(parts_table, parts_no, NULL);
}

static void parts_state_free(struct parts_state *state)
{
	// Маска попиксельного hit-теста привязана к текстуре состояния, которую этот
	// switch и удаляет — иначе после смены CG остался бы кэш от прежней картинки.
	free(state->common.hit_mask);
	state->common.hit_mask = NULL;
	state->common.hit_mask_w = state->common.hit_mask_h = 0;
	switch (state->type) {
	case PARTS_UNINITIALIZED:
	case PARTS_RECT_DETECTION:
	case PARTS_LAYOUT_BOX:
		break;
	case PARTS_CG:
	case PARTS_CG_DETECTION:
		gfx_delete_texture(&state->common.texture);
		if (state->cg.name)
			free_string(state->cg.name);
		break;
	case PARTS_TEXT:
		parts_text_free(&state->text);
		break;
	case PARTS_ANIMATION:
		for (unsigned i = 0; i < state->anim.nr_frames; i++) {
			gfx_delete_texture(&state->anim.frames[i]);
		}
		free(state->anim.frames);
		break;
	case PARTS_NUMERAL:
	case PARTS_PANEL:
		gfx_delete_texture(&state->common.texture);
		break;
	case PARTS_HGAUGE:
	case PARTS_VGAUGE:
		gfx_delete_texture(&state->common.texture);
		gfx_delete_texture(&state->gauge.cg);
		break;
	case PARTS_CONSTRUCTION_PROCESS:
		gfx_delete_texture(&state->common.texture);
		parts_clear_construction_process(&state->cproc);
		break;
	case PARTS_FLASH:
		parts_flash_free(&state->flash);
		break;
	case PARTS_FLAT:
		parts_flat_free(&state->flat);
		break;
	case PARTS_MOVIE:
		if (state->movie.apeg) {
			// Ролик нового API: текстура наша, её и сносим.
			apeg_movie_close(state->movie.apeg);
			state->movie.apeg = NULL;
			gfx_delete_texture(&state->common.texture);
			break;
		}
		// The texture is owned by the SACT sprite.
		state->common.texture.handle = 0;
		if (state->movie.sprite_no >= 0)
			sact_SP_Delete(state->movie.sprite_no);
		break;
	case PARTS_3DLAYER:
		state->common.texture.handle = 0;
		if (state->layer3d.plugin >= 0)
			ReignEngine_ReleasePlugin(state->layer3d.plugin);
		if (state->layer3d.sprite_no >= 0)
			sact_SP_Delete(state->layer3d.sprite_no);
		break;
	}
	memset(state, 0, sizeof(struct parts_state));
}

static struct text_style default_text_style = {
	.face = FONT_GOTHIC,
	.size = 16.0f,
	.bold_width = 0.0f,
	.weight = FW_NORMAL,
	.edge_left = 0.0f,
	.edge_up = 0.0f,
	.edge_right = 0.0f,
	.edge_down = 0.0f,
	.color = { .r = 255, .g = 255, .b = 255, .a = 255 },
	.edge_color = { .r = 0, .g = 0, .b = 0, .a = 255 },
	.scale_x = 1.0f,
	.space_scale_x = 1.0f,
	.font_spacing = 0.0f,
	.font_size = NULL
};

void parts_state_reset(struct parts_state *state, enum parts_type type)
{
	/*
	 * XSYS4_STATE_TRACE=1 — КТО ПЕРЕБИВАЕТ УЖЕ ЗАПОЛНЕННОЕ СОСТОЯНИЕ. Смена типа
	 * рушит содержимое (`parts_state_free`), а происходит она НЕЯВНО: любой
	 * `parts_get_text`/`parts_get_cg` на части «не того» типа молча сбрасывает её.
	 * Ловушка на этот случай: печатаем со стеком вызовов игры только переход
	 * ИЗ инициализированного состояния В другой тип.
	 */
	if (state->type != PARTS_UNINITIALIZED && state->type != type
			&& getenv("XSYS4_STATE_TRACE")) {
		NOTICE("STATE перебив: тип %d -> %d", state->type, type);
		vm_stack_trace();
	}
	parts_state_free(state);
	state->type = type;
	switch (type) {
	case PARTS_TEXT:
		state->text.ts = default_text_style;
		break;
	case PARTS_NUMERAL:
		state->num.length = 1;
		state->num.font_no = -1;
		// Шрифтовый режим (`表示タイプ = 2`) пользуется этим стилем; нулевой
		// scale_x из memset дал бы нулевую ширину знака.
		state->num.ts = default_text_style;
		break;
	case PARTS_CONSTRUCTION_PROCESS:
		TAILQ_INIT(&state->cproc.ops);
		break;
	case PARTS_HGAUGE:
	case PARTS_VGAUGE:
		state->gauge.cg_no = -1;
		state->gauge.rate = 0.0f;
		break;
	case PARTS_3DLAYER:
		state->layer3d.plugin = -1;
		state->layer3d.sprite_no = -1;
		break;
	case PARTS_UNINITIALIZED:
	case PARTS_CG:
	case PARTS_CG_DETECTION:
	case PARTS_ANIMATION:
	case PARTS_FLASH:
	case PARTS_FLAT:
	case PARTS_RECT_DETECTION:
		break;
	case PARTS_MOVIE:
		state->movie.sprite_no = -1;
		state->movie.apeg = NULL;
		break;
	case PARTS_LAYOUT_BOX:
		state->layout_box.layout_type = PARTS_LAYOUT_VERTICAL;
		state->layout_box.align = 1;
		break;
	case PARTS_PANEL:
		// Непрозрачный чёрный до первого SetPanelColor.
		state->panel.color = (SDL_Color) { 0, 0, 0, 255 };
		break;
	}
}

struct parts_cg *parts_get_cg(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_CG) {
		parts_state_reset(&parts->states[state], PARTS_CG);
	}
	return &parts->states[state].cg;
}

struct parts_text *parts_get_text(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_TEXT) {
		parts_state_reset(&parts->states[state], PARTS_TEXT);
	}
	return &parts->states[state].text;
}

struct parts_animation *parts_get_animation(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_ANIMATION) {
		parts_state_reset(&parts->states[state], PARTS_ANIMATION);
	}
	return &parts->states[state].anim;
}

struct parts_numeral *parts_get_numeral(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_NUMERAL) {
		parts_state_reset(&parts->states[state], PARTS_NUMERAL);
	}
	return &parts->states[state].num;
}

struct parts_gauge *parts_get_hgauge(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_HGAUGE) {
		parts_state_reset(&parts->states[state], PARTS_HGAUGE);
	}
	return &parts->states[state].gauge;
}

struct parts_gauge *parts_get_vgauge(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_VGAUGE) {
		parts_state_reset(&parts->states[state], PARTS_VGAUGE);
	}
	return &parts->states[state].gauge;
}

struct parts_construction_process *parts_get_construction_process(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_CONSTRUCTION_PROCESS) {
		parts_state_reset(&parts->states[state], PARTS_CONSTRUCTION_PROCESS);
	}
	return &parts->states[state].cproc;
}

struct parts_flash *parts_get_flash(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_FLASH) {
		parts_state_reset(&parts->states[state], PARTS_FLASH);
	}
	return &parts->states[state].flash;
}

struct parts_flat *parts_get_flat(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_FLAT) {
		parts_state_reset(&parts->states[state], PARTS_FLAT);
	}
	return &parts->states[state].flat;
}

struct parts_movie *parts_get_movie(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_MOVIE) {
		parts_state_reset(&parts->states[state], PARTS_MOVIE);
	}
	return &parts->states[state].movie;
}

struct parts_layout_box *parts_get_layout_box(struct parts *parts)
{
	if (parts->states[0].type != PARTS_LAYOUT_BOX) {
		parts_state_reset(&parts->states[0], PARTS_LAYOUT_BOX);
	}
	return &parts->states[0].layout_box;
}

struct parts_3dlayer *parts_get_3dlayer(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_3DLAYER) {
		parts_state_reset(&parts->states[state], PARTS_3DLAYER);
	}
	return &parts->states[state].layer3d;
}

static Point calculate_offset(int mode, int w, int h)
{
	switch (mode) {
	case 1:  return (Point) {    0, 0    }; // top-left
	case 2:  return (Point) { -w/2, 0    }; // top-center
	case 3:  return (Point) {   -w, 0    }; // top-right
	case 4:  return (Point) {    0, -h/2 }; // middle-left
	case 5:  return (Point) { -w/2, -h/2 }; // middle-center
	case 6:  return (Point) {   -w, -h/2 }; // middle-right
	case 7:  return (Point) {    0, -h   }; // bottom-left
	case 8:  return (Point) { -w/2, -h   }; // bottom-center
	case 9:  return (Point) {   -w, -h   }; // bottom-right
	default: return (Point) { mode, (3*h)/4 }; // why...
	}
}

/*
 * Should be called when:
 *   - position (parts->pos) changes
 *   - width or height changes
 *   - origin mode changes
 */
static void parts_common_recalculate_hitbox(struct parts *parts, struct parts_common *common)
{

	if (common->surface_area.w || common->surface_area.h) {
		common->origin_offset = calculate_offset(parts->origin_mode,
				common->surface_area.w, common->surface_area.h);
		Rectangle r = { 0, 0, common->w, common->h };
		SDL_IntersectRect(&r, &common->surface_area, &common->hitbox);
		common->origin_offset.x -= common->surface_area.x;
		common->origin_offset.y -= common->surface_area.y;
		common->hitbox.x += parts->local.pos.x + common->origin_offset.x;
		common->hitbox.y += parts->local.pos.y + common->origin_offset.y;
	} else {
		common->origin_offset = calculate_offset(parts->origin_mode, common->w, common->h);
		common->hitbox = (Rectangle) {
			.x = parts->local.pos.x + common->origin_offset.x,
			.y = parts->local.pos.y + common->origin_offset.y,
			.w = common->w,
			.h = common->h,
		};
	}
	// У окна реплик служебная часть текста стоит от УГЛА окна, а не от его точки
	// привязки, поэтому её позицию надо пересчитывать вместе с origin_offset —
	// то есть при смене размера, позиции и 原点座標モード. Это единственная воронка,
	// через которую проходят все три события.
	if (parts->mw)
		parts_message_window_relayout(parts);
}

void parts_recalculate_hitbox(struct parts *parts)
{
	for (int i = 0; i < PARTS_NR_STATES; i++) {
		parts_common_recalculate_hitbox(parts, &parts->states[i].common);
	}
}

/*
 * Позиция ребёнка складывается с УЧЁТОМ масштаба родителя: в иерархии
 * трансформаций масштаб сжимает не только сам рисунок, но и смещения детей.
 *
 * ★Без этого уменьшенная сцена «разъезжалась»: обучение Dohna кладёт сцену под
 * родителя со `拡大縮小 = 0.85` (`Tutorial::MoveSceneParent` / SceneParentStack) —
 * каждая часть рисовалась мельче, но на СВОЁМ прежнем месте, поэтому кадр не
 * собирался в уменьшенную картинку (правый край уезжал за экран), а голубые
 * рамки-подсветки, которые обводят элемент, к которому относится подсказка,
 * не совпадали с этим элементом.
 */
/*
 * Часть-ЯКОРЬ: своего вида у неё нет (`w == h == 0`), но задан `原点座標モード` ≠ 1,
 * и тогда точка привязки относится к её СОДЕРЖИМОМУ — весь поддеревом сдвигается
 * так, чтобы указанный угол содержимого попал в заданную координату.
 *
 * Живой случай (пункт 17 доводки Dohna): панель `TP / ROOMS / TALENT / GARAGE`
 * в правом верхнем углу экрана Garage. В раскладке `SceneGarage.x` это часть
 * `GarageInfo` — `ユーザコンポーネント` в (1258, 19) с `原点座標モード = 3`
 * (привязка по ПРАВОМУ краю). Своего размера у неё нет, содержимое приходит из
 * отдельной активности `GarageInformation`, поэтому `calculate_offset` по её
 * собственным нулям давал сдвиг 0, и панель уезжала вправо ровно на свою ширину:
 * замер по розовой заливке — у оригинала панель занимает x 893–1250, у нас от неё
 * оставался уголок 1255–1273, а ширина содержимого 362 ≈ величина промаха.
 *
 * ★Раскладочные боксы (`パーツタイプ = レイアウトボックス` с непустым типом) СЮДА НЕ
 * ПОПАДАЮТ: у них тот же `原点座標モード` уже учитывает `parts_do_layout`
 * (`align_offset_*`), и второй сдвиг был бы двойным счётом. В диалоге FEEL таких
 * боксов с origin 7 и нулевым размером несколько — там всё считает раскладка.
 */
static Point parts_anchor_shift(struct parts *parts)
{
	// Откат для замеров A/B на одном бинаре: XSYS4_NO_ANCHOR_SHIFT=1 — часть без
	// своего вида поддерево не двигает (поведение до этой правки).
	static const char *off = (const char *)1;
	if (off == (const char *)1)
		off = getenv("XSYS4_NO_ANCHOR_SHIFT");
	if (off && *off)
		return (Point) { 0, 0 };
	if (!parts || parts->origin_mode == 1)
		return (Point) { 0, 0 };
	/*
	 * ★ТОЛЬКО `ユーザコンポーネント` (место под чужую активность): у него содержимое
	 * И ЕСТЬ вид части, поэтому точку привязки осмысленно отсчитывать по нему.
	 * Ради такого случая правка и делалась — `GarageInfo` у Dohna.
	 *
	 * Обычный контейнер, созданный САМОЙ ИГРОЙ, сюда попадать не должен: его
	 * потомки расставлены абсолютными координатами, и сдвиг уводит всё поддерево.
	 * Живой случай — ADV-сцена Haha Ranman: часть 1000001035 (`origin=5`, свой
	 * размер 0×0, содержимое 1000×500, стоит в центре экрана 640,360) получала
	 * сдвиг -500,-250, и сцена съезжала влево-вверх — на экране от неё оставался
	 * прямоугольник 780×470 в левом верхнем углу («игра не на весь экран»).
	 * Найдено ручкой XSYS4_NO_ANCHOR_SHIFT + XSYS4_ANCHOR_TRACE.
	 *
	 * Прежнее широкое поведение для замеров: XSYS4_ANCHOR_SHIFT_ANY=1.
	 *
	 * ★КОРЕНЬ АКТИВНОСТИ — второй законный случай, и по той же причине: своего
	 * вида у него нет, а содержимое — вся активность. `原点座標モード` у корня в
	 * раскладках Dohna не встречается НИ РАЗУ (все 195 корней — режим 1), его
	 * ставит код игры, и единственный такой сеттер во всём байткоде —
	 * `CustomerView@OriginPosMode::set(4)` (`IActivity@Root::get` →
	 * `IParts@OriginPosMode::set`) для карточек клиентов на экране наград
	 * hustling. Без сдвига карточка уезжала вниз ровно на половину своей высоты:
	 * `ResultCustomerViewCollection@SetPartsPos` ставит y = 360, что при режиме 4
	 * (середина-лево) есть вертикальный ЦЕНТР карточки, а у нас это был её верх
	 * (кадр пользователя: верх карточки ~370 против ~118 у оригинала). Контейнер
	 * Haha Ranman (часть 1000001035), ради которого сужение и делалось, корнем
	 * активности не является — его создаёт сама игра. FINDINGS §5dz, пункт 1.
	 * Откат: XSYS4_ANCHOR_ACT_ROOT=0.
	 */
	static const char *any = (const char *)1;
	if (any == (const char *)1)
		any = getenv("XSYS4_ANCHOR_SHIFT_ANY");
	static const char *act_root = (const char *)1;
	if (act_root == (const char *)1)
		act_root = getenv("XSYS4_ANCHOR_ACT_ROOT");
	bool root_ok = parts->is_activity_root && !(act_root && act_root[0] == '0');
	if (!parts->is_user_component && !root_ok && !(any && *any))
		return (Point) { 0, 0 };
	struct parts_state *state = &parts->states[parts->state];
	if (state->common.w || state->common.h)
		return (Point) { 0, 0 };
	if (state->type == PARTS_LAYOUT_BOX
			&& state->layout_box.layout_type != PARTS_LAYOUT_FREE)
		return (Point) { 0, 0 };
	int w = 0, h = 0;
	parts_get_content_size(parts, &w, &h);
	if (!w && !h)
		return (Point) { 0, 0 };
	Point shift = calculate_offset(parts->origin_mode, w, h);
	/*
	 * ★СДВИГ ПРИВЯЗКИ МАСШТАБИРУЕТСЯ ВМЕСТЕ С СОДЕРЖИМЫМ. Размер содержимого
	 * `parts_get_content_size` считает по ЛОКАЛЬНЫМ координатам потомков, а на
	 * экране те стоят с масштабом самой части (`parts_update_global_pos` умножает
	 * смещение ребёнка на `global.scale` родителя). Значит содержимое занимает
	 * `w·scale`, и точка привязки обязана отсчитываться по нему же — иначе край,
	 * за который часть привязана, уезжает на `w·(scale−1)`.
	 *
	 * Живой случай (FINDINGS §5dz, пункт 2): плашка денег `MoneyView` на экране
	 * наград hustling стоит в (1264, 12) с `原点座標モード = 3` (привязка за ПРАВЫЙ
	 * край) и при начислении РАСТЁТ (motion масштабирует её). С немасштабированным
	 * сдвигом правый край уходил за экран — на кадре пользователя сумма обрезана
	 * рамкой окна, у оригинала плашка кончается на 1264.
	 * Откат: XSYS4_ANCHOR_NO_SCALE=1.
	 */
	static const char *noscale = (const char *)1;
	if (noscale == (const char *)1)
		noscale = getenv("XSYS4_ANCHOR_NO_SCALE");
	if (!(noscale && *noscale)) {
		shift.x = (int)roundf(shift.x * parts->global.scale.x);
		shift.y = (int)roundf(shift.y * parts->global.scale.y);
	}
	/*
	 * XSYS4_ANCHOR_TRACE=1 — назвать части, которым сдвиг реально достаётся (по
	 * разу на номер): номер, режим привязки, размер содержимого и сам сдвиг, плюс
	 * признак `ユーザコンポーネント` и родитель. Нужен, когда после этой правки
	 * СЦЕНА уезжает целиком: по строке видно, какая часть увела поддерево и на
	 * сколько — сдвиг на половину содержимого выглядит как «экран не во весь рост».
	 */
	static const char *tr = (const char *)1;
	static int seen[256], nr_seen = 0;
	if (tr == (const char *)1)
		tr = getenv("XSYS4_ANCHOR_TRACE");
	if (tr && *tr && (shift.x || shift.y) && nr_seen < 256) {
		bool dup = false;
		for (int i = 0; i < nr_seen; i++)
			if (seen[i] == parts->no) dup = true;
		if (!dup) {
			seen[nr_seen++] = parts->no;
			NOTICE("ANCHOR part=%d origin=%d содержимое=%dx%d сдвиг=%d,%d uc=%d parent=%d pos=%d,%d",
			       parts->no, parts->origin_mode, w, h, shift.x, shift.y,
			       parts->is_user_component,
			       parts->parent ? parts->parent->no : -1,
			       parts->global.pos.x, parts->global.pos.y);
		}
	}
	return shift;
}

// Точка, ОТ КОТОРОЙ отсчитываются потомки: позиция части плюс её сдвиг привязки.
static Point parts_child_origin(struct parts *parts)
{
	Point shift = parts_anchor_shift(parts);
	/*
	 * ★В ЗЕРКАЛЬНОЙ системе координат (часть стоит под развёрнутым контейнером)
	 * сдвиг привязки МЕНЯЕТ ЗНАК. Это не догадка, а следствие правила «поддерево
	 * отражается целиком»: точка отсчёта потомков — точка, и её образ относительно
	 * позиции части равен −сдвиг. (У КОРОБКИ части правило другое, −(сдвиг+размер),
	 * потому что у неё есть протяжённость, — см. parts_origin_offset_x.)
	 *
	 * Живого случая у Dohna нет: сдвиг привязки достаётся только
	 * частям-`ユーザコンポーネント` без своего размера, а в зеркальных поддеревьях боя
	 * стоят лишь CG-части и контейнеры с `原点座標モード` 1/8 (замер: дамп 837
	 * частей, из них 60 зеркальных). Правило внесено, чтобы модель была
	 * непротиворечивой, а не чтобы «сошлось» на одной сцене.
	 */
	if (parts->global.mirror_x)
		shift.x = -shift.x;
	if (parts->global.mirror_y)
		shift.y = -shift.y;
	return (Point) { parts->global.pos.x + shift.x, parts->global.pos.y + shift.y };
}

/*
 * Зеркальность системы координат, в которой стоят ПОТОМКИ части: своя плюс
 * унаследованная (XOR). Именно её `parts_update_global_pos` передаёт детям.
 */
static bool parts_children_mirror_x(struct parts *parts)
{
	return parts->global.mirror_x != parts->reverse_lr;
}

static bool parts_children_mirror_y(struct parts *parts)
{
	return parts->global.mirror_y != parts->reverse_tb;
}

// Зеркальность системы координат, в которой стоит САМА часть (от предков).
static bool parts_frame_mirror_x(struct parts *parts)
{
	return parts->parent ? parts_children_mirror_x(parts->parent) : false;
}

static bool parts_frame_mirror_y(struct parts *parts)
{
	return parts->parent ? parts_children_mirror_y(parts->parent) : false;
}

static void parts_update_global_pos(struct parts *parts, Point parent_pos,
		float parent_scale_x, float parent_scale_y,
		bool mirror_x, bool mirror_y)
{
	/*
	 * ★Смещение ребёнка ПОВОРАЧИВАЕТСЯ вместе с родителем: наклон родителя — это
	 * жёсткое преобразование всего поддерева, а не «каждый повернулся у себя».
	 * Без этого подпись узла данжа Dohna наклонялась правильно, а её розовая
	 * бирка (`M11`, ребёнок подписи со смещением ~15 px по x) оставалась на
	 * прежнем месте и отрывалась от текста — на кадре бирки уезжали правее и ниже
	 * подписей (§5dx). Угол берём накопленный (`global`), тот же, с которым
	 * родитель рисуется.
	 */
	float rot = parts->parent ? parts->parent->global.rotation.z : 0.0f;
	float lx = parts->local.pos.x * parent_scale_x;
	float ly = parts->local.pos.y * parent_scale_y;
	/*
	 * ★В ЗЕРКАЛЬНОЙ системе координат смещение потомка меняет знак: разворот
	 * контейнера — это отражение всего поддерева вокруг его точки отсчёта, а не
	 * переворот каждой картинки на своём месте. Без этого разворот, который игра
	 * ставит рамке кадра бойца (`wh=0x0`, картинки нет), не давал ВООБЩЕ ничего:
	 * враги Dohna смотрели вправо, как нарисован ассет, а не влево, как в
	 * оригинале (§5ej).
	 *
	 * Зеркало применяется ДО поворота, а угол берётся с обратным знаком:
	 * `M∘R(θ) = R(−θ)∘M`. У боевого поддерева Dohna углы нулевые, но правило
	 * должно быть точным, иначе наклонённая зеркальная группа поедет.
	 */
	parts->global.mirror_x = mirror_x;
	parts->global.mirror_y = mirror_y;
	if (mirror_x)
		lx = -lx;
	if (mirror_y)
		ly = -ly;
	if (mirror_x != mirror_y)
		rot = -rot;
	if (rot != 0.0f) {
		float a = rot * (float)M_PI / 180.0f;
		float c = cosf(a), s = sinf(a);
		float rx = lx * c - ly * s;
		float ry = lx * s + ly * c;
		lx = rx;
		ly = ry;
	}
	parts->global.pos = (Point) {
		parent_pos.x + (int)roundf(lx),
		parent_pos.y + (int)roundf(ly)
	};

	Point child_origin = parts_child_origin(parts);
	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_pos(child, child_origin,
				parts->global.scale.x, parts->global.scale.y,
				parts_children_mirror_x(parts),
				parts_children_mirror_y(parts));
	}
}

// Масштаб родителя для позиционирования САМОЙ части (её собственный масштаб
// влияет только на детей, см. parts_update_global_pos).
static float parts_parent_scale_x(struct parts *parts)
{
	return parts->parent ? parts->parent->global.scale.x : 1.0f;
}

static float parts_parent_scale_y(struct parts *parts)
{
	return parts->parent ? parts->parent->global.scale.y : 1.0f;
}

static void parts_reposition_family(struct parts *parts)
{
	parts_update_global_pos(parts,
			parts->parent ? parts_child_origin(parts->parent) : root_pos,
			parts_parent_scale_x(parts), parts_parent_scale_y(parts),
			parts_frame_mirror_x(parts), parts_frame_mirror_y(parts));
}

void parts_set_pos(struct parts *parts, Point pos)
{
	parts->local.pos.x = pos.x;
	parts->local.pos.y = pos.y;
	parts_recalculate_hitbox(parts);
	parts_reposition_family(parts);
	parts_dirty(parts);
}

void parts_set_global_pos(Point pos)
{
	root_pos = pos;
	struct parts *parts;
	PARTS_LIST_FOREACH(parts) {
		// Позиционную семантику этого обхода не меняем (он гоняет ВЕСЬ список,
		// не только корни), но зеркальность берём по настоящему предку — иначе
		// глобальный сдвиг сбрасывал бы зеркало поддерева.
		parts_update_global_pos(parts, root_pos, 1.0f, 1.0f,
				parts_frame_mirror_x(parts), parts_frame_mirror_y(parts));
	}
	parts_engine_dirty();
}

static void parts_update_global_z(struct parts *parts, int parent_z)
{
	parts->global.z = parent_z + parts->local.z;
	parts_list_resort(parts);

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_z(child, parts->global.z);
	}
}

void parts_set_z(struct parts *parts, int z)
{
	if (parts->local.z == z)
		return;

	parts->local.z = z;
	parts_update_global_z(parts, parts->parent ? parts->parent->global.z : 0);
	parts_dirty(parts);
}

static void parts_update_global_show(struct parts *parts, bool parent_show)
{
	parts->global.show = parent_show && parts->local.show;

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_show(child, parts->global.show);
	}
}

void parts_set_show(struct parts *parts, bool show)
{
	if (parts->local.show == show)
		return;

	parts->local.show = show;
	parts_update_global_show(parts, parts->parent ? parts->parent->global.show : true);
	parts_dirty(parts);
}

static void parts_update_global_alpha(struct parts *parts, int parent_alpha)
{
	parts->global.alpha = parent_alpha * (parts->local.alpha / 255.0f);

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_alpha(child, parts->global.alpha);
	}
}

void parts_set_alpha(struct parts *parts, int alpha)
{
	if (getenv("XSYS4_MOTION_TRACE") && parts->no >= 90000000) {
		static int nc = 0;
		if ((nc++ % 20) == 0 || alpha == 255 || alpha == 0)
			NOTICE("ALPHA set part %d local=%d->%d parent_g=%d", parts->no,
			       parts->local.alpha, max(0, min(255, alpha)),
			       parts->parent ? parts->parent->global.alpha : 255);
	}
	parts->local.alpha = max(0, min(255, alpha));
	parts_update_global_alpha(parts, parts->parent ? parts->parent->global.alpha : 255);
	parts_dirty(parts);
}

/*
 * ★ДОБАВОЧНЫЙ ЦВЕТ СКЛАДЫВАЕТСЯ, А НЕ УМНОЖАЕТСЯ. Здесь была копия соседней
 * `parts_update_global_multiply_color` (апстрим, `fc42c42` 2022) — `parent * (local/255)`.
 * У умножения нейтральный элемент 255, у сложения — 0, поэтому прежняя формула гасила
 * добавку ВСЕГДА: у части без своего цвета выходило `parent * 0 = 0`, а у части без
 * родителя `parts_set_add_color` подставляет корню `{0,0,0}` — и `0 * local = 0`. То
 * есть `SetAddColor`/`SetComponentAddColor` не делали ничего ни для одной игры.
 *
 * Живой случай: на карте Haha Ranman маркер события у панели `Shrine` должен МЕРЦАТЬ
 * БЕЛЫМ. Игра считает это сама (`CBlink(0, 64.0, 1500, 500)` — добавка 0…64 за 1.5 с
 * с паузой 0.5 с) и каждый кадр кладёт её на РОДИТЕЛЬСКИЙ контейнер маркера; замер
 * (`XSYS4_MUL_TRACE=1`, лог `playtest/haharanman-blink-fresh.log`): у части `90000558`
 * добавочный цвет честно ходит `0 → 63 → 0`, а на экране не менялось ни одного пикселя.
 *
 * Складываем с потолком 255: клампить обязательно, иначе на цепочке вложенных
 * контейнеров сумма переполнит байт.
 */
static void parts_update_global_add_color(struct parts *parts, SDL_Color parent_color)
{
	parts->global.add_color = (SDL_Color) {
		min(255, parent_color.r + parts->local.add_color.r),
		min(255, parent_color.g + parts->local.add_color.g),
		min(255, parent_color.b + parts->local.add_color.b),
		0
	};

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_add_color(child, parts->global.add_color);
	}
}

void parts_set_add_color(struct parts *parts, SDL_Color color)
{
	parts->local.add_color = color;
	parts_update_global_add_color(parts, parts->parent ? parts->parent->global.add_color
			: (SDL_Color){0,0,0,0});
	parts_dirty(parts);
}

static void parts_update_global_multiply_color(struct parts *parts, SDL_Color parent_color)
{
	parts->global.multiply_color = (SDL_Color) {
		parent_color.r * (parts->local.multiply_color.r / 255.0f),
		parent_color.g * (parts->local.multiply_color.g / 255.0f),
		parent_color.b * (parts->local.multiply_color.b / 255.0f),
		255
	};

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_multiply_color(child, parts->global.multiply_color);
	}
}

void parts_set_multiply_color(struct parts *parts, SDL_Color color)
{
	parts->local.multiply_color = color;
	parts_update_global_multiply_color(parts, parts->parent ? parts->parent->global.multiply_color
			: (SDL_Color){255,255,255,255});
	parts_dirty(parts);
}

void parts_set_origin_mode(struct parts *parts, int origin_mode)
{
	parts->origin_mode = origin_mode;
	parts_recalculate_hitbox(parts);
	parts_dirty(parts);
}

static void parts_update_global_scale_x(struct parts *parts, float parent_scale_x)
{
	parts->global.scale.x = parent_scale_x * parts->local.scale.x;

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_scale_x(child, parts->global.scale.x);
	}
}

void parts_set_scale_x(struct parts *parts, float mag)
{
	parts->local.scale.x = mag;
	parts_recalculate_hitbox(parts);
	parts_update_global_scale_x(parts, parts_parent_scale_x(parts));
	parts_reposition_family(parts);  // смещения детей зависят от масштаба
	parts_dirty(parts);
}

static void parts_update_global_scale_y(struct parts *parts, float parent_scale_y)
{
	parts->global.scale.y = parent_scale_y * parts->local.scale.y;

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_scale_y(child, parts->global.scale.y);
	}
}

void parts_set_scale_y(struct parts *parts, float mag)
{
	parts->local.scale.y = mag;
	parts_recalculate_hitbox(parts);
	parts_update_global_scale_y(parts, parts_parent_scale_y(parts));
	parts_reposition_family(parts);
	parts_dirty(parts);
}

/*
 * ★РАЗВОРОТ ПО X/Y НАСЛЕДУЕТСЯ ПОДДЕРЕВОМ, как и плоский поворот по Z: у Dohna его
 * ставят контейнеру без картинки (`Base` в `StandView`, 131×577, tex=0), а видимое
 * висит на нём детьми. ТОЧКУ ОСИ здесь не храним — рендер берёт её обходом вверх
 * (`parts_rot3d_origin`): хранимое поле пришлось бы синхронизировать ещё и при
 * перемещении части, во втором пути расчёта параметров и при загрузке сейва, а
 * каждый пропуск давал бы поворот вокруг угла экрана.
 */
static void parts_update_global_rotate_x(struct parts *parts, float parent_rot_x)
{
	parts->global.rotation.x = parent_rot_x + parts->local.rotation.x;

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_rotate_x(child, parts->global.rotation.x);
	}
}

void parts_set_rotation_x(struct parts *parts, float rot)
{
	parts->local.rotation.x = rot;
	parts_update_global_rotate_x(parts, parts->parent ? parts->parent->global.rotation.x : 0.0f);
	parts_dirty(parts);
}

static void parts_update_global_rotate_y(struct parts *parts, float parent_rot_y)
{
	parts->global.rotation.y = parent_rot_y + parts->local.rotation.y;

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_rotate_y(child, parts->global.rotation.y);
	}
}

void parts_set_rotation_y(struct parts *parts, float rot)
{
	parts->local.rotation.y = rot;
	parts_update_global_rotate_y(parts, parts->parent ? parts->parent->global.rotation.y : 0.0f);
	parts_dirty(parts);
}

static void parts_update_global_rotate_z(struct parts *parts, float parent_rot_z)
{
	parts->global.rotation.z = parent_rot_z + parts->local.rotation.z;

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_rotate_z(child, parts->global.rotation.z);
	}
}

// XSYS4_ROT_TRACE=1 — кто и на какой угол разворачивает части. Нужна не «для полноты»:
// у наклонных фигур (метка времени на колесе знаков карты Haha Ranman) по кадру не
// отличить «игра угол не отдала» от «угол пришёл, но не доехал до отрисовки».
void parts_set_rotation_z(struct parts *parts, float rot)
{
	if (getenv("XSYS4_ROT_TRACE"))
		NOTICE("ROT set z part %d: %.2f -> %.2f (родитель %d, глобальный %.2f)",
		       parts->no, parts->local.rotation.z, rot,
		       parts->parent ? parts->parent->no : -1,
		       parts->parent ? parts->parent->global.rotation.z : 0.0f);
	parts->local.rotation.z = rot;
	parts_update_global_rotate_z(parts, parts->parent ? parts->parent->global.rotation.z : 0.0f);
	// Смещения потомков считаются в ПОВЁРНУТОЙ системе родителя
	// (parts_update_global_pos), поэтому смена угла двигает поддерево.
	parts_reposition_family(parts);
	parts_dirty(parts);
}

/*
 * Размер части ИЗМЕНИЛСЯ — раскладка предков-боксов, посчитанная по прежнему
 * размеру, устарела. Сам по себе `parts_dirty` этого не чинит: он лишь просит
 * перерисовать кадр, а `parts_do_layout` выполняется только для узлов, попавших
 * в `dirty_list` через `parts_component_dirty`.
 *
 * Живой случай — бейдж вариантов на карточке экрана CG Haha Ranman
 * (`ＣＧモード／項目`, бокс `差分` с детьми `数字：枚数` · `ＣＧ：スラッシュ` ·
 * `数字：最大枚数`). Порядок такой: активность строится с однозначными числами,
 * бокс раскладывает трёх детей шагом 12 px (один глиф), и лишь потом игра ставит
 * реальные значения `SetNumeralNumber(10)` и `(65)`. Числовые части стали по
 * 24 px, а позиции остались от шага 12 — второй глиф каждого числа накрывал
 * соседа, и слэш пропадал под цифрой: на экране `1065` вместо `10 / 65`.
 * Замер (`XSYS4_DUMP_PARTS`): у видимой карточки `wh=24x18 / 12x18 / 24x18`
 * при позициях `-18 / -6 / +6`, тогда как у карточек, чьи числа были известны
 * ДО раскладки, — `-30 / -6 / +6`, то есть по фактическим ширинам.
 *
 * Помечаем ВСЮ цепочку предков-боксов, а не только прямого родителя: контейнер
 * без своего вида меряется по содержимому (`parts_get_content_size`), поэтому
 * изменение размера внука меняет и размер деда.
 */
static void parts_invalidate_ancestor_layout(struct parts *parts)
{
	// Откат для замеров: XSYS4_NO_RELAYOUT_ON_RESIZE=1 — прежнее поведение.
	if (getenv("XSYS4_NO_RELAYOUT_ON_RESIZE"))
		return;
	for (struct parts *p = parts->parent; p; p = p->parent) {
		if (p->states[0].type != PARTS_LAYOUT_BOX)
			continue;
		if (p->states[0].layout_box.layout_type == PARTS_LAYOUT_FREE)
			continue;
		parts_component_dirty(p);
	}
}

void parts_set_dims(struct parts *parts, struct parts_common *common, int w, int h)
{
	bool resized = common->w != w || common->h != h;
	common->w = w;
	common->h = h;
	parts_common_recalculate_hitbox(parts, common);
	if (resized)
		parts_invalidate_ancestor_layout(parts);
}

bool _parts_cg_set(struct parts *parts, struct parts_cg *parts_cg, struct cg *cg, int cg_no,
		struct string *name)
{
	if (!cg)
		return false;
	if (getenv("XSYS4_CG_TRACE") && parts->no >= 90000000) {
		long nzc = 0, nza = 0; long n = (long)cg->metrics.w * cg->metrics.h;
		if (cg->pixels) {
			uint8_t *p = cg->pixels;
			for (long i = 0; i < n; i++) {
				if (p[i*4] || p[i*4+1] || p[i*4+2]) nzc++;
				if (p[i*4+3]) nza++;
			}
		}
		NOTICE("CGLOAD part=%d no=%d name='%s' type=%d %dx%d bpp=%d haspix=%d hasalpha=%d nonzeroRGB=%ld nonzeroA=%ld/%ld",
		       parts->no, cg_no, name ? display_sjis0(name->text) : "(nil)",
		       cg->type, cg->metrics.w, cg->metrics.h, cg->metrics.bpp,
		       cg->metrics.has_pixel, cg->metrics.has_alpha, nzc, nza, n);
	}
	gfx_delete_texture(&parts_cg->common.texture);
	gfx_init_texture_with_cg(&parts_cg->common.texture, cg);
	parts_set_dims(parts, &parts_cg->common, cg->metrics.w, cg->metrics.h);
	parts_cg->no = cg_no;
	if (parts_cg->name)
		free_string(parts_cg->name);
	parts_cg->name = name;
	parts_dirty(parts);
	cg_free(cg);
	return true;
}

bool parts_cg_set_by_index(struct parts *parts, struct parts_cg *cg, int cg_no)
{
	assert(cg_no);
	return _parts_cg_set(parts, cg, asset_cg_load(cg_no), cg_no, NULL);
}

bool parts_cg_set(struct parts *parts, struct parts_cg *parts_cg, struct string *cg_name)
{
	assert(cg_name && *(cg_name->text) != '\0');
	int no;
	struct cg *cg;
	if (!memcmp(cg_name->text, "<save>", 6)) {
		char *path = savedir_path(cg_name->text + 6);
		uint32_t t0 = SDL_GetTicks();
		cg = cg_load_file(path);
		// Под XSYS4_XPE_TRACE видно цену <save>-картинок: они идут МИМО кэша CG
		// (файл в папке сейвов, не ассет) и на листании scrollback декодируются
		// с диска каждый раз.
		if (getenv("XSYS4_XPE_TRACE"))
			NOTICE("CG <save> '%s' %dx%d за %u ms", cg_name->text + 6,
			       cg ? cg->metrics.w : -1, cg ? cg->metrics.h : -1,
			       SDL_GetTicks() - t0);
		no = 0;
		free(path);
	} else {
		cg = asset_cg_load_by_name(cg_name->text, &no);
	}
	return _parts_cg_set(parts, parts_cg, cg, no, string_dup(cg_name));
}

struct parts_numeral_font *parts_numeral_fonts = NULL;
int parts_nr_numeral_fonts = 0;

void parts_numeral_font_init(struct parts_numeral_font *font)
{
	if (font->type == PARTS_NUMERAL_FONT_SEPARATE) {
		for (int i = 0; i < 12; i++) {
			struct cg *cg = asset_cg_load(font->cg_no + i);
			if (!cg) {
				font->cg[i].handle = 0;
				continue;
			}
			gfx_init_texture_with_cg(&font->cg[i], cg);
			cg_free(cg);
		}
	} else if (font->type == PARTS_NUMERAL_FONT_SEPARATE2) {
		for (int i = 0; i < 12; i++) {
			if (font->width[i] < 0)
				continue;
			struct cg *cg = asset_cg_load(font->width[i]);
			if (!cg) {
				font->cg[i].handle = 0;
				continue;
			}
			gfx_init_texture_with_cg(&font->cg[i], cg);
			cg_free(cg);
		}
	} else if (font->type == PARTS_NUMERAL_FONT_COMBINED) {
		int x = 0;
		Texture t = {0};
		struct cg *cg = asset_cg_load(font->cg_no);
		gfx_init_texture_with_cg(&t, cg);
		for (int i = 0; i < 12; i++) {
			if (font->width[i] <= 0)
				continue;
			gfx_init_texture_blank(&font->cg[i], font->width[i], t.h);
			gfx_copy_with_alpha_map(&font->cg[i], 0, 0, &t, x, 0, font->width[i], t.h);
			x += font->width[i];
		}
		gfx_delete_texture(&t);
		free(cg);
	}
}

static int parts_load_numeral_font_separate(int cg_no)
{
	// find existing font
	for (int i = 0; i < parts_nr_numeral_fonts; i++) {
		if (parts_numeral_fonts[i].type == PARTS_NUMERAL_FONT_SEPARATE
				&& parts_numeral_fonts[i].cg_no == cg_no)
			return i;
	}

	// load new font
	int font_no = parts_nr_numeral_fonts++;
	parts_numeral_fonts = xrealloc_array(parts_numeral_fonts, font_no, font_no + 1,
			sizeof(struct parts_numeral_font));
	struct parts_numeral_font *font = &parts_numeral_fonts[font_no];
	font->cg_no = cg_no;
	font->type = PARTS_NUMERAL_FONT_SEPARATE;
	parts_numeral_font_init(font);
	return font_no;
}

static int parts_load_numeral_font_separate_string(struct string *cg_name)
{
	// convert name to CG indices
	int indices[12];
	for (int i = 0; i < 12; i++) {
		struct string *name = string_format(cg_name, (union vm_value){.i = i}, STRFMT_INT);
		if (!asset_exists_by_name(ASSET_CG, name->text, &indices[i])) {
			WARNING("numeral cg doesn't exist: %s", display_sjis0(name->text));
			indices[i] = -1;
		}
		free_string(name);
	}

	// find existing font
	for (int i = 0; i < parts_nr_numeral_fonts; i++) {
		struct parts_numeral_font *font = &parts_numeral_fonts[i];
		if (font->type != PARTS_NUMERAL_FONT_SEPARATE2)
			continue;
		bool not_match = false;
		for (int d = 0; d < 12; d++) {
			if (font->width[d] != indices[d]) {
				not_match = true;
				break;
			}
		}
		if (not_match)
			continue;
		return i;
	}

	// load new font
	int font_no = parts_nr_numeral_fonts++;
	parts_numeral_fonts = xrealloc_array(parts_numeral_fonts, font_no, font_no + 1,
			sizeof(struct parts_numeral_font));
	struct parts_numeral_font *font = &parts_numeral_fonts[font_no];
	font->cg_no = indices[0];
	font->type = PARTS_NUMERAL_FONT_SEPARATE2;
	memcpy(font->width, indices, sizeof(int) * 12);
	parts_numeral_font_init(font);
	return font_no;
}

static int parts_load_numeral_font_combined(struct cg *cg, int cg_no, int w[12])
{
	// find existing font
	for (int i = 0; i < parts_nr_numeral_fonts; i++) {
		struct parts_numeral_font *font = &parts_numeral_fonts[i];
		if (font->type != PARTS_NUMERAL_FONT_COMBINED || font->cg_no != cg_no)
			continue;
		for (int i = 0; i < 12; i++) {
			if (w[i] != font->width[i])
				continue;
		}
		return i;
	}

	// load new font
	int font_no = parts_nr_numeral_fonts++;
	parts_numeral_fonts = xrealloc_array(parts_numeral_fonts, font_no, font_no + 1,
			sizeof(struct parts_numeral_font));
	struct parts_numeral_font *font = &parts_numeral_fonts[font_no];
	font->cg_no = cg_no;
	font->type = PARTS_NUMERAL_FONT_COMBINED;
	memcpy(font->width, w, sizeof(int) * 12);
	parts_numeral_font_init(font);
	return font_no;
}

/*
 * Число, нарисованное ШРИФТОМ (`表示タイプ = 2`): цифры берутся не из набора CG,
 * а из обычного шрифта с параметрами `SetNumeralFont`. У Dohna так сделаны все
 * счётчики интерфейса — «TALENT 3/3» и «Client 1/4» на экране подбора талантов,
 * счётчик клиентов и т.д.; движок этот режим не знал вовсе, и на месте чисел
 * оставались пустые места (сверено с оригиналом на экране Hustling).
 *
 * `字間隔` (num->space) прибавляется к шагу каждого знака, как у текстовой части;
 * запятая и минус — обычные символы шрифта, отдельных CG им не нужно.
 */
static bool parts_numeral_font_update(struct parts *parts, struct parts_numeral *num)
{
	char buf[64];
	int_least64_t n = num->num;
	int len = 0;
	if (num->show_comma) {
		char digits[32];
		int nd = snprintf(digits, sizeof(digits), "%" PRIdLEAST64, n < 0 ? -n : n);
		if (n < 0)
			buf[len++] = '-';
		for (int i = 0; i < nd && len < (int)sizeof(buf) - 2; i++) {
			if (i > 0 && (nd - i) % 3 == 0)
				buf[len++] = ',';
			buf[len++] = digits[i];
		}
		buf[len] = '\0';
	} else if (num->length > 0) {
		len = snprintf(buf, sizeof(buf), "%0*" PRIdLEAST64, num->length, n);
	} else {
		len = snprintf(buf, sizeof(buf), "%" PRIdLEAST64, n);
	}
	if (len <= 0)
		return true;

	int h = text_style_height(&num->ts);
	float total = 0;
	for (int i = 0; i < len; i++) {
		char ch[3] = { buf[i], '\0', '\0' };
		total += text_style_width(&num->ts, ch) + num->space;
	}
	int w = max(1, (int)ceilf(total - num->space));
	if (getenv("XSYS4_NUMTEXT_TRACE")) {
		char first[3] = { buf[0], 0, 0 };
		NOTICE("NUMW part=%d \"%s\" face=%u size=%.1f bold=%.2f edge=%.2f,%.2f space=%d adv_first=%.2f -> w=%d h=%d",
		       parts->no, buf, num->ts.face, num->ts.size, num->ts.bold_width,
		       num->ts.edge_left, num->ts.edge_right, num->space,
		       gfx_size_char(&num->ts, first), w, h);
	}

	gfx_delete_texture(&num->common.texture);
	gfx_init_texture_rgba(&num->common.texture, w, h, (SDL_Color){0, 0, 0, 0});
	float x = 0;
	for (int i = 0; i < len; i++) {
		char ch[3] = { buf[i], '\0', '\0' };
		x += gfx_render_textf(&num->common.texture, x, 0, ch, &num->ts, false);
		x += num->space;
	}
	parts_set_dims(parts, &num->common, w, h);
	return true;
}

static bool parts_numeral_update(struct parts *parts, struct parts_numeral *num)
{
	// XXX: don't generate texture if number hasn't been set yet
	if (!num->have_num)
		return true;
	if (num->use_font)
		return parts_numeral_font_update(parts, num);
	if (num->font_no < 0)
		return true;
	int_least64_t n = num->num;
	bool negative = n < 0;
	if (negative)
		n *= -1;

	// extract digits
	int digits;
	uint8_t d[32];
	for (digits = 0; n; digits++) {
		d[digits] = n % 10;
		n /= 10;
	}

	// encode number as texture indices
	int nr_chars = 0;
	uint8_t chars[32];
	if (negative) {
		chars[nr_chars++] = 10;
	}
	for (int i = 0; i < digits; i++) {
		if (num->show_comma && i > 0 && i % 3 == 0) {
			chars[nr_chars++] = 11;
		}
		chars[nr_chars++] = d[i];
	}

	for (int i = digits; i < num->length; i++) {
		chars[nr_chars++] = 0;
	}

	struct parts_numeral_font *font = &parts_numeral_fonts[num->font_no];

	// determine output dimensions
	int w = 0, h = 0;
	for (int i = nr_chars-1; i >= 0; i--) {
		if (!font->cg[chars[i]].handle)
			continue;
		w += font->cg[chars[i]].w;
		h = max(h, font->cg[chars[i]].h);
	}
	w += (nr_chars-1) * num->space;

	// copy chars to texture
	gfx_delete_texture(&num->common.texture);
	gfx_init_texture_rgba(&num->common.texture, w, h, (SDL_Color){0, 0, 0, 255});

	int x = 0;
	for (int i = nr_chars-1; i>= 0; i--) {
		Texture *ch = &font->cg[chars[i]];
		if (!ch->handle)
			continue;
		gfx_copy_with_alpha_map(&num->common.texture, x, 0, ch, 0, 0, ch->w, ch->h);
		x += ch->w + num->space;
	}

	parts_set_dims(parts, &num->common, w, h);

	parts_dirty(parts);
	return true;

}

bool parts_numeral_set_number(struct parts *parts, struct parts_numeral *num, int n)
{
	num->have_num = true;
	num->num = n;
	return parts_numeral_update(parts, num);
}

void parts_set_state(struct parts *parts, enum parts_state_type state)
{
	if (parts->lock_input_state)
		return;
	while (state > PARTS_STATE_DEFAULT && parts->states[state].type == PARTS_UNINITIALIZED)
		state--;
	if (parts->state != state) {
		parts->state = state;
		parts_dirty(parts);
	}
}

void parts_set_surface_area(struct parts *parts, struct parts_common *common, int x, int y, int w, int h)
{
	if (x < 0 || y < 0)
		return;
	common->surface_area = (Rectangle) { x, y, w, h };
	parts_common_recalculate_hitbox(parts, common);
}

static bool parts_animation_update(struct parts_animation *anim, int passed_time)
{
	if (passed_time <= 0 || !anim->nr_frames)
		return false;

	const unsigned elapsed = anim->elapsed + passed_time;
	const unsigned frame_diff = elapsed / anim->frame_time;
	const unsigned remainder = elapsed % anim->frame_time;

	if (frame_diff > 0) {
		anim->elapsed = remainder;
		anim->current_frame = (anim->current_frame + frame_diff) % anim->nr_frames;
		anim->common.texture = anim->frames[anim->current_frame];
		return true;
	} else {
		anim->elapsed = elapsed;
		return false;
	}
}

/* Ролик тикает по тем же часам, что анимация: догнать кадр и, если он сменился,
 * залить его в текстуру части. */
static bool parts_movie_update(struct parts_movie *movie)
{
	if (!movie->apeg || !apeg_movie_update(movie->apeg))
		return false;
	gfx_update_texture_with_pixels(&movie->common.texture,
				       (void *)apeg_movie_pixels(movie->apeg));
	return true;
}

static void parts_update_loop(struct parts *parts, int passed_time)
{
	bool dirty = false;
	switch (parts->states[parts->state].type) {
	case PARTS_ANIMATION:
		dirty = parts_animation_update(&parts->states[parts->state].anim, passed_time);
		break;
	case PARTS_FLASH:
		dirty = parts_flash_update(&parts->states[parts->state].flash, passed_time);
		break;
	case PARTS_FLAT:
		dirty = parts_flat_update(&parts->states[parts->state].flat, passed_time);
		break;
	case PARTS_MOVIE:
		dirty = parts_movie_update(&parts->states[parts->state].movie);
		break;
	default:
		break;
	}
	// Посимвольное проявление реплики идёт по тому же тику: окно реплик держит фон
	// обычным состоянием части, поэтому в switch выше его ветки нет.
	if (parts->mw && parts_message_window_update(parts, passed_time))
		dirty = true;
	if (dirty)
		parts_dirty(parts);
}

static void parts_update_animation(int passed_time)
{
	struct parts *parts;
	PARTS_LIST_FOREACH(parts) {
		parts_update_loop(parts, passed_time);
	}
}

void parts_release(int parts_no)
{
	struct ht_slot *slot = ht_put_int(parts_table, parts_no, NULL);
	if (!slot->value)
		return;
	if (getenv("XSYS4_CTRL_TRACE") || getenv("XSYS4_PARTS_TRACE") || parts_watched(parts_no))
		NOTICE("parts_release(%d)", parts_no);
	if (parts_watched(parts_no)) {
		NOTICE("PARTWATCH освобождение парта %d — стек вызовов игры:", parts_no);
		vm_stack_trace();
	}

	struct parts *parts = slot->value;
	// Уходя, снимаем свою ссылку с маски: иначе счётчик никогда не дойдёт до нуля
	// и часть, побывавшая маской, останется невидимой навсегда (фон CONFIG).
	if (parts->alpha_clipper_parts_no) {
		struct parts *clip = parts_try_get(parts->alpha_clipper_parts_no);
		if (clip && --clip->alpha_clipper_refs <= 0) {
			clip->alpha_clipper_refs = 0;
			clip->is_alpha_clipper = false;
			parts_dirty(clip);
		}
	}
	parts_input_reset_drag(parts);
	parts_clear_motion(parts);
	parts_message_window_free(parts->mw);
	parts->mw = NULL;
	for (int i = 0; i < PARTS_NR_STATES; i++) {
		parts_state_free(&parts->states[i]);
	}

	// break parent/child relationships
	// ★Пробовали «Release забирает всё ПОДДЕРЕВО» (рекурсивно освобождать детей) —
	// плёночный шум титула в углу пролога это НЕ убрало (он воскресает покадровым
	// тиком мёртвой сцены, см. §5c), а семантику меняет заметно: у игры остаются
	// живые обёртки на детей. Не возвращать без замера, который это подтвердит.
	while (!TAILQ_EMPTY(&parts->children)) {
		struct parts *child = TAILQ_FIRST(&parts->children);
		TAILQ_REMOVE(&parts->children, child, child_list_entry);
		child->parent = NULL;
	}
	if (parts->parent) {
		TAILQ_REMOVE(&parts->parent->children, parts, child_list_entry);
		parts->parent = NULL;
	}

	free(parts->radio_children);
	parts->radio_children = NULL;
	parts->nr_radio_children = 0;

	parts_list_remove(parts);
	dirty_list_remove(parts);
	free(parts);
	slot->value = NULL;
	parts_engine_dirty();
}

/*
 * Заявка на разовый дамп (см. parts_request_debug_dump в parts.h). Флаг ставит
 * обработчик сигнала, снимает и исполняет — обработчик ввода в главном цикле.
 */
static volatile sig_atomic_t dump_requested = 0;

void parts_request_debug_dump(void)
{
	dump_requested = 1;
}

bool parts_take_debug_dump_request(void)
{
	if (!dump_requested)
		return false;
	dump_requested = 0;
	return true;
}

void parts_debug_dump(void)
{
	struct parts *p;
	int n = 0;
	/*
	 * `XSYS4_DETACH_TEST=<номер>` — ПРОВЕРКА ОТВЯЗКИ на живом дереве. Ни одна из
	 * четырёх игр не зовёт `ClearParent`/`RemoveChild` из игрового кода (только из
	 * редактора активностей и анкеты), поэтому семантику `Parent::set(0)` иначе
	 * нечем предъявить. По ПЕРВОМУ `kill -USR1` заявляем отвязку указанной части,
	 * по второму — в дампе у неё уже `parent=-1`, а прежний родитель не считает её
	 * своим потомком.
	 */
	{
		static bool detach_done = false;
		const char *dt = getenv("XSYS4_DETACH_TEST");
		if (dt && !detach_done) {
			detach_done = true;
			int no = atoi(dt);
			struct parts *t = parts_try_get(no);
			NOTICE("  DETACH_TEST: заявлена отвязка части %d (родитель сейчас %d)",
			       no, t && t->parent ? t->parent->no : -1);
			if (t)
				PE_SetParentPartsNumber(no, 0);
		}
	}
	{
		char buf[256]; int m = 0;
		for (int i = 0; i < ctrl_stack.nr_controllers && m < 200; i++)
			m += snprintf(buf + m, sizeof(buf) - m, "%d%s ", ctrl_stack.stack[i],
				      ctrl_stack.hidden[i] ? "(скрыт)" : "");
		buf[m] = 0;
		NOTICE("  СТЕК СЛОЁВ (низ→верх): %s| активный %d", buf, ctrl_stack.active);
	}
	// Реестр активностей — рядом со стеком слоёв: часть, числящаяся за живой
	// активностью, свой слой ПЕРЕЖИВАЕТ (см. `keep_act` в PE_RemoveController), и
	// без этой сводки «остаток на экране» не отличить от «игра не позвала
	// ReleaseActivity».
	pe_dump_activities();
	PARTS_LIST_FOREACH(p) {
		Rectangle *hb = &p->states[0].common.hitbox;
		// Имя CG у состояния 0 — без него в дампе не отличить «наш прямоугольник не
		// оттуда» от «игра так и задумала»: у Haha Ranman на ADV-сцене поверх фона
		// висели белый и серый прямоугольники, и опознать их можно было только по CG.
		const char *cgname = "";
		if (p->states[0].type == PARTS_CG || p->states[0].type == PARTS_CG_DETECTION) {
			if (p->states[0].cg.name)
				cgname = display_sjis0(p->states[0].cg.name->text);
		}
		// Активность, за которой числится часть (пусто — часть создана игрой).
		const char *actname = pe_parts_activity_name(p->no);
		NOTICE("  dump part %d: act=\"%s\" ctrl=%d lshow=%d gshow=%d z=%d pos=%d,%d st0type=%d parent=%d hovered=%d state=%d hitbox=%d,%d,%dx%d wh=%dx%d origin=%d mul=%d,%d,%d add=%d,%d,%d%s rev=%d%d mir=%d%d scale=%.2f,%.2f rotz=%.2f/%.2f rotxy=%.2f,%.2f pass=%d eip=%d clk=%d btn=%d cmask=%d alpha=%d lhid=%d mw=%d link=%d clip=%d isclip=%d tex=%u cg=\"%s\"",
		       p->no, actname ? actname : "", p->controller_no,
		       p->local.show, p->global.show, p->global.z,
		       p->global.pos.x, p->global.pos.y, p->states[0].type, p->parent ? p->parent->no : -1,
		       p->is_hovered, p->state, hb->x, hb->y, hb->w, hb->h,
		       p->states[0].common.w, p->states[0].common.h, p->origin_mode,
		       p->global.multiply_color.r, p->global.multiply_color.g, p->global.multiply_color.b,
		       // Добавочный цвет — рядом с mul: цвет на экране получается ими ОБОИМИ
		       // (`tex*mul + add`), и по одному mul объяснить кадр нельзя — «труба»
		       // маршрута в данже из белой поверхности выходит светло-розовой именно
		       // добавочным цветом (§5dw).
		       p->global.add_color.r, p->global.add_color.g, p->global.add_color.b,
		       p->sub_color_mode ? "(sub)" : "",
		       // ★Зеркалирование — рядом с масштабом: «спрайт смотрит не в ту
		       // сторону» надо различать как «флаг стоит, а рисуем без отражения»
		       // против «флаг не выставлен вовсе», а по кадру это одно и то же.
		       // `mir=` — зеркальность системы координат, УНАСЛЕДОВАННАЯ от предков
		       // (разворот контейнера). Без неё «rev=00 у спрайта» читается как
		       // «зеркала нет», хотя отражать его обязан предок.
		       p->reverse_lr, p->reverse_tb,
		       p->global.mirror_x, p->global.mirror_y,
		       p->global.scale.x, p->global.scale.y,
		       p->local.rotation.z, p->global.rotation.z,
		       p->local.rotation.x, p->local.rotation.y,
		       p->pass_cursor, p->enable_input_process, p->clickable, p->is_button, p->construction_mask,
		       p->global.alpha, parts_hidden_by_layer(p), p->message_window,
		       p->linked_to, p->alpha_clipper_parts_no, p->is_alpha_clipper,
		       p->states[p->state].common.texture.handle, cgname);
		n++;
	}
	NOTICE("parts_debug_dump: %d parts total", n);
}

void parts_release_all(void)
{
	while (!TAILQ_EMPTY(&parts_list)) {
		struct parts *parts = TAILQ_FIRST(&parts_list);
		parts_release(parts->no);
	}

	for (int i = 0; i < parts_nr_numeral_fonts; i++) {
		struct parts_numeral_font *font = &parts_numeral_fonts[i];
		for (int i = 0; i < 12; i++) {
			if (font->cg[i].handle)
				gfx_delete_texture(&font->cg[i]);
		}
	}

	free(parts_numeral_fonts);
	parts_numeral_fonts = NULL;
	parts_nr_numeral_fonts = 0;
}

static bool parts_engine_initialized = false;

void PE_enable_multi_controller(void)
{
	if (parts_multi_controller)
		return;
	assert(!parts_engine_initialized);
	parts_multi_controller = true;
}

bool PE_Init(void)
{
	if (parts_engine_initialized)
		return true;
	// XXX: Oyako Rankan doesn't call ChipmunkSpriteEngine.Init
	sact_init(16, CHIPMUNK_SPRITE_ENGINE);
	parts_table = ht_create(1024);
	parts_render_init();
	parts_debug_init();
	ctrl_stack_init();
	parts_engine_initialized = true;
	return true;
}

void PE_Reset(void)
{
	if (pending_ctype_table) {
		ht_foreach_value(pending_ctype_table, free);
		ht_free_int(pending_ctype_table);
		pending_ctype_table = NULL;
	}
	PE_ReleaseAllParts();
	PE_ReleaseMessage();
	parts_input_reset();
	ctrl_stack_init();
	sact_ModuleFini();
}

static bool parts_has_dirty_parent(struct parts *parts)
{
	for (struct parts *parent = parts->parent; parent; parent = parent->parent) {
		if (parent->dirty)
			return true;
	}
	return false;
}

static void parts_combine_params(struct parts_params *parent, struct parts_params *child,
		struct parts_params *out, bool mirror_x, bool mirror_y)
{
	out->z = parent->z + child->z;
	/*
	 * ★Смещение ребёнка МАСШТАБИРУЕТСЯ родителем — ровно как в
	 * parts_update_global_pos, куда это правило внесли раньше. Здесь его не было,
	 * и два пути расчёта расходились: позиция, посчитанная при установке
	 * координат, затиралась несмасштабированной при ближайшем UpdateComponent.
	 *
	 * Замер, на котором поймано: образец реплики в превью страницы `Text Area UI`
	 * у Dohna. Окно `メッセージウィンドウサンプル` — 862×206 с масштабом 0.66 и
	 * центральным origin, его текстовая часть стоит локально в (-301,-41).
	 * Правильно 832 + (-301 × 0.66) = 633, у оригинала текст и начинается с 633;
	 * без масштаба выходило 832 - 301 = 531, и текст вылезал за левый край рамки
	 * окна на картинку сцены.
	 */
	/*
	 * ★И ЗЕРКАЛО — тоже в ОБОИХ путях: в зеркальной системе координат предков
	 * смещение потомка меняет знак (см. parts_update_global_pos и
	 * parts_params::mirror_x). Если внести правило только туда, ближайший
	 * `UpdateComponent` вернёт незеркальную позицию, и разворот врагов Dohna будет
	 * то появляться, то исчезать.
	 */
	out->mirror_x = mirror_x;
	out->mirror_y = mirror_y;
	float dx = child->pos.x * parent->scale.x;
	float dy = child->pos.y * parent->scale.y;
	if (mirror_x)
		dx = -dx;
	if (mirror_y)
		dy = -dy;
	/*
	 * ★И ПОВОРОТ — тоже в ОБОИХ путях. Наклон родителя разворачивает смещение
	 * потомка (правило и его доказательство — в parts_update_global_pos, §5dx:
	 * розовая бирка `M11` обязана держаться за наклонной подписью узла данжа).
	 * Здесь его не было, и ближайший `UpdateComponent` возвращал НЕповёрнутое
	 * смещение — то есть §5dx воспроизводился заново на любом пересчёте.
	 *
	 * Замер (узел `Parking Ent.`, подпись 90000967 наклонена на 323.75°, бирка
	 * 90000969 смещена на (84,-14)): в живой игре бирка стоит в (744,272), после
	 * загрузки образа сейва вставала в (769,319) — ровно неповёрнутое смещение.
	 * Поворот на −36.25° даёт (59,-61) и возвращает бирку в (744,272).
	 *
	 * Порядок как в первом пути: зеркало применяется ДО поворота, а угол берётся
	 * с обратным знаком при зеркальности по одной оси (`M∘R(θ) = R(−θ)∘M`).
	 */
	float rot = parent->rotation.z;
	if (mirror_x != mirror_y)
		rot = -rot;
	if (rot != 0.0f) {
		float a = rot * (float)M_PI / 180.0f;
		float c = cosf(a), s = sinf(a);
		float rx = dx * c - dy * s;
		float ry = dx * s + dy * c;
		dx = rx;
		dy = ry;
	}
	out->pos = (Point) {
		parent->pos.x + (int)roundf(dx),
		parent->pos.y + (int)roundf(dy)
	};
	out->show = parent->show && child->show;
	out->alpha = parent->alpha * (child->alpha / 255.0f);
	out->scale.x = parent->scale.x * child->scale.x;
	out->scale.y = parent->scale.y * child->scale.y;
	out->rotation.x = parent->rotation.x + child->rotation.x;
	out->rotation.y = parent->rotation.y + child->rotation.y;
	out->rotation.z = parent->rotation.z + child->rotation.z;
	/*
	 * ★ДОБАВОЧНЫЙ ЦВЕТ СКЛАДЫВАЕТСЯ — та же модель, что в
	 * `parts_update_global_add_color` (там она и доказана: мерцающий белым маркер
	 * события на карте Haha Ranman). Здесь оставалась апстримная умножительная
	 * формула, и ВТОРОЙ путь расчёта затирал первый на ближайшем
	 * `UpdateComponent`: у части с родителем добавка гасилась в ноль
	 * (`parent.add = 0` у корня → `0 * local = 0`) — ровно та ловушка «правило
	 * нужно в ОБА пути», о которой предупреждает §5ej для позиции.
	 * Клампим, иначе на цепочке контейнеров сумма переполнит байт.
	 */
	out->add_color.r = min(255, parent->add_color.r + child->add_color.r);
	out->add_color.g = min(255, parent->add_color.g + child->add_color.g);
	out->add_color.b = min(255, parent->add_color.b + child->add_color.b);
	out->multiply_color.r = parent->multiply_color.r * (child->multiply_color.r / 255.0f);
	out->multiply_color.g = parent->multiply_color.g * (child->multiply_color.g / 255.0f);
	out->multiply_color.b = parent->multiply_color.b * (child->multiply_color.b / 255.0f);
}

// Передать слой всему поддереву: у детей он тоже определяется местом в дереве.
static void parts_inherit_controller(struct parts *parts, int controller_no)
{
	if (parts->controller_no == controller_no)
		return;
	parts->controller_no = controller_no;
	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts)
		parts_inherit_controller(child, controller_no);
}

static void parts_update_component(struct parts *parts)
{
	if (parts->parent) {
		parts_combine_params(&parts->parent->global, &parts->local, &parts->global,
				parts_children_mirror_x(parts->parent),
				parts_children_mirror_y(parts->parent));
		// Позиция считается ДВУМЯ путями (здесь и в parts_update_global_pos), и
		// сдвиг привязки родителя-якоря нужен в обоих: иначе один путь затирает
		// другой при ближайшем UpdateComponent. Ровно на этом правка сперва и не
		// подействовала — панель Garage осталась за кромкой.
		// ...и знак сдвига в зеркальной системе координат — тоже в обоих
		// (parts_child_origin, где то же правило выведено подробно).
		Point shift = parts_anchor_shift(parts->parent);
		if (parts->parent->global.mirror_x)
			shift.x = -shift.x;
		if (parts->parent->global.mirror_y)
			shift.y = -shift.y;
		parts->global.pos.x += shift.x;
		parts->global.pos.y += shift.y;
	}
	if (parts_get_sprite_z(parts) != parts->sp.z
			|| parts_get_sprite_z2(parts) != parts->sp.z2) {
		parts_list_resort(parts);
	}

	if (parts->dirty) {
		TAILQ_REMOVE(&dirty_list, parts, dirty_list_entry);
		parts->dirty = false;
	}

	parts_do_layout(parts);

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_component(child);
	}
}

void PE_UpdateComponent(possibly_unused int passed_time)
{
	// After loading a save, the game script may compute a negative time delta
	// because it stores an absolute system.GetTime() value in the save data,
	// which is meaningless across process restarts.
	if (passed_time < 0)
		passed_time = 0;
	while (!TAILQ_EMPTY(&dirty_list)) {
		// pop parts object from dirty list
		struct parts *parts = TAILQ_FIRST(&dirty_list);
		TAILQ_REMOVE(&dirty_list, parts, dirty_list_entry);
		parts->dirty = false;

		// update parent
		// Сам на себя: при оригинальной семантике GetFreeNumber (скан с алиасингом
		// «пустых» выдач) игра штатно делает SetParentPartsNumber(X, X) — узлы её
		// собственного дерева алиасятся в один парт. Подвесить парт к самому себе
		// нельзя (цикл в children ⇒ бесконечная рекурсия обхода) — игнорируем.
		if (parts->pending_parent == parts->no)
			parts->pending_parent = -1;
		struct parts *parent;
		// Цикл предков: подвесить парт к собственному потомку нельзя — обход
		// детей (CallUserComponentEventWithChild) уходит в бесконечную
		// рекурсию (переполнение call_stack после загрузки сейва).
		if (parts->pending_parent > 0) {
			struct parts *anc = parts_try_get(parts->pending_parent);
			for (; anc; anc = anc->parent) {
				if (anc == parts) {
					WARNING("парт %d: родитель %d — его же потомок, связь отвергнута",
					        parts->no, parts->pending_parent);
					parts->pending_parent = -1;
					break;
				}
			}
		}
		// ОТВЯЗКА (`Parent::set(0)` / `RemoveChild`): рвём связь с родителем, слой
		// части не трогаем — она остаётся на своём экране, просто ничья. Раньше
		// сюда приходил -1 («изменений нет») или 0 (несуществующая часть 0), и то
		// и другое молча вырождалось в no-op.
		if (parts->pending_parent == 0 && parts->parent) {
			struct parts *old = parts->parent;
			TAILQ_REMOVE(&old->children, parts, child_list_entry);
			parts->parent = NULL;
			if (getenv("XSYS4_PARTS_TRACE") || parts_watched(parts->no))
				NOTICE("PARTS   парт %d отвязан от %d", parts->no, old->no);
			if (old->states[0].type == PARTS_LAYOUT_BOX)
				parts_component_dirty(old);
		}
		if (parts->pending_parent > 0 && (parent = parts_try_get(parts->pending_parent))) {
			if (parts->parent) {
				TAILQ_REMOVE(&parts->parent->children, parts, child_list_entry);
			}
			parts->parent = parent;
			TAILQ_INSERT_TAIL(&parent->children, parts, child_list_entry);
			// Слой (контроллер) определяется МЕСТОМ В ДЕРЕВЕ, а не тем, какой
			// слой был активен в момент создания части: переподчинение и есть
			// способ перенести часть на другой экран. Раньше `controller_no`
			// оставался от создания, и часть навсегда сортировалась по ЧУЖОМУ
			// слою — а при multi-controller слой это ПЕРВИЧНЫЙ ключ порядка
			// (parts_get_sprite_z), z — только вторичный.
			// Живой случай: системные кнопки ADV у Haha Ranman создаются на слое
			// 0 (титул), затем подвешиваются к частям окна сообщений на слое 1.
			// Оставаясь на слое 0, они уходили ПОД непрозрачную рамку окна и
			// пропадали с экрана — при том что show, alpha, текстура и координаты
			// у них были верные (проверено дампом). Пересортировку делать не надо:
			// `parts_update_component` ниже сам заметит смену ключа.
			parts_inherit_controller(parts, parent->controller_no);

			// if parent is layout box, mark it dirty so that it can re-layout its children
			if (parent->states[0].type == PARTS_LAYOUT_BOX)
				parts_component_dirty(parent);
		}
		// TODO: should the child be orphaned if it already has a parent and an invalid
		//       parent no is given?
		parts->pending_parent = -1;

		// don't do anything here if parent is dirty
		if (parts_has_dirty_parent(parts))
			continue;

		// update child params
		parts_update_component(parts);
	}
}

bool parts_message_window_show = true;

void PE_Update(int passed_time, bool message_window_show)
{
	parts_message_window_show = message_window_show;
	PE_UpdateComponent(passed_time);
	audio_update();
	parts_update_animation(passed_time);
	PE_UpdateInputState(passed_time);
	parts_render_update();
}

void PE_UpdateParts(int passed_time, possibly_unused bool is_skip, bool message_window_show)
{
	parts_message_window_show = message_window_show;
	audio_update();
	parts_update_animation(passed_time);
	parts_render_update();
}

void PE_SetDelegateIndex(int parts_no, int delegate_index)
{
	if (getenv("XSYS4_ACT_TRACE"))
		NOTICE("ACT SetDelegateIndex(parts=%d, delegate=%d)", parts_no, delegate_index);
	parts_get(parts_no)->delegate_index = delegate_index;
}

int PE_GetDelegateIndex(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->delegate_index : -1;
}

// Ixseal-форма привязки обработчика к части. Имена аргументов взяты из .ain
// (`ainfnsig` по обёртке `parts::detail::SetEventID` fno 9144):
// `SetEventID(partsNumber, delegateIndex, uniqueID)` — то есть это и есть
// «SetDelegateIndex» новых игр, только вместе с идентификатором набора
// обработчиков. Оба значения обязательны: `CPartsMessageManager@CallDelegate`
// (@0x2c6eb6) сначала требует, чтобы `GetMessageDelegateIndex` был валидным
// индексом в его списке наборов, а затем — чтобы `GetMessageUniqueID` совпал с
// `GetUniqueID` найденного набора; иначе он молча возвращает false, и клик по
// кнопке не диспатчится (так на титуле Dohna не работал ни один пункт меню).
void PE_SetEventID(int parts_no, int delegate_index, int unique_id)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return;
	if (getenv("XSYS4_ACT_TRACE"))
		NOTICE("ACT SetEventID(parts=%d, delegate=%d, uid=%d)",
		       parts_no, delegate_index, unique_id);
	parts->delegate_index = delegate_index;
	parts->event_unique_id = unique_id;
}

// Номер части, чей CG совпал с XSYS4_CG_WATCH: за ней потом следят PE_SetShow и
// PE_ReleaseParts — так видно, гасит/освобождает ли её игра вообще. Читается
// через parts_cg_watch_part(), чтобы условие наблюдения было ОДНО (parts_watched).
static int cg_watch_part = -1;
int parts_cg_watch_part(void) { return cg_watch_part; }

bool PE_SetPartsCG(int parts_no, struct string *cg_name, int sprite_deform, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	parts->sprite_deform = sprite_deform;
	// XSYS4_CG_WATCH=<подстрока имени CG> — ОДНОРАЗОВЫЙ стек вызовов игры на
	// первой установке подходящего CG: отвечает «кто эту картинку кладёт».
	{
		static bool cg_watch_done = false;
		const char *w = getenv("XSYS4_CG_WATCH");
		// Имя CG хранится в SJIS, а подстрока из env — в UTF-8: сравниваем с
		// ПЕРЕКОДИРОВАННЫМ именем, иначе не совпадёт никогда.
		if (w && *w && cg_name) {
			const char *utf8 = display_sjis0(cg_name->text);
			if (strstr(utf8, w)) {
				// Строка — на КАЖДОЕ совпадение, и обязательно со СЛОЕМ: так
				// ищется «чьи парты остались на экране» (у Haha Ranman на
				// ADV-сцене висели чужие — титульная плёнка и рамка `行動選択`).
				// Номера партов `1000001xxx` выдаются на лету и от прогона к
				// прогону РАЗНЫЕ, поэтому искать их по номеру бесполезно.
				NOTICE("CGWATCH part=%d слой %d cg='%s'",
				       parts_no, parts->controller_no, utf8);
				// Стек вызовов игры — ОДИН раз: он отвечает «кто её кладёт».
				if (!cg_watch_done) {
					cg_watch_done = true;
					cg_watch_part = parts_no;
					NOTICE("CGWATCH стек вызовов игры:");
					vm_stack_trace();
				}
			}
		}
	}
	if (!cg_name || *(cg_name->text) == '\0') {
		// Сброс CG раньше не логировался вовсе — «тихое» обнуление затёртого
		// парта не находилось трейсом (чёрный фон пролога Dohna).
		if (getenv("XSYS4_PARTS_TRACE"))
			NOTICE("PARTS SetPartsCG(%d, '') -> reset", parts_no);
		parts_state_reset(&parts->states[state], PARTS_CG);
		parts_dirty(parts);
		return true;
	}

	struct parts_cg *cg = parts_get_cg(parts, state);
	bool ok = parts_cg_set(parts, cg, cg_name);
	if (getenv("XSYS4_PARTS_TRACE"))
		NOTICE("PARTS SetPartsCG(%d, '%s') -> %d", parts_no, display_sjis0(cg_name->text), ok);
	return ok;
}

bool PE_SetPartsCG_by_index(int parts_no, int cg_no, int sprite_deform, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	parts->sprite_deform = sprite_deform;
	if (!cg_no) {
		parts_state_reset(&parts->states[state], PARTS_CG);
		parts_dirty(parts);
		return true;
	}

	struct parts_cg *cg = parts_get_cg(parts, state);
	return parts_cg_set_by_index(parts, cg, cg_no);
}

// XXX: Rance Quest
bool PE_SetPartsCG_by_string_index(int parts_no, struct string *cg_name,
		int sprite_deform, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	parts->sprite_deform = sprite_deform;
	if (!cg_name) {
		parts_state_reset(&parts->states[state], PARTS_CG);
		parts_dirty(parts);
		return true;
	}

	struct parts_cg *cg = parts_get_cg(parts, state);
	int cg_no = atoi(cg_name->text);
	if (cg_no) {
		if (!parts_cg_set_by_index(parts, cg, atoi(cg_name->text)))
			return false;
		cg->name = string_ref(cg_name);
		return true;
	} else if (!memcmp(cg_name->text, "<save>SaveData\\", 15)) {
		char *path = savedir_path(cg_name->text + 15);
		bool result = _parts_cg_set(parts, cg, cg_load_file(path), 0, string_ref(cg_name));
		free(path);
		return result;
	} else {
		VM_ERROR("Invalid CG name: %s", display_sjis0(cg_name->text));
	}
}

void PE_GetPartsCGName(int parts_no, struct string **cg_name, int state)
{
	if (!parts_state_valid(--state))
		return;
	struct parts_cg *cg = parts_get_cg(parts_get(parts_no), state);
	if (cg->name) {
		if (*cg_name)
			free_string(*cg_name);
		*cg_name = string_dup(cg->name);
	}
}

/*
 * Пара к четвёртому аргументу PE_SetPartsCG (`sprite_deform`): 0 — без
 * искажения, 1 — отражение по горизонтали, 2 — по вертикали (так его и
 * трактует render_parts). Сеттер значение хранит по-настоящему и оно даже
 * попадает в сейв, а геттер отсутствовал — на нём вставала ADV-сцена Dohna:
 * `CCGParts@ReverseLR::get` — это ровно `Deform::get() == 1`
 * (@0x2fc18a: CALLMETHOD Deform; PUSH 1; EQUALE), и его читает
 * `CCGParts@CGName::set`, чтобы восстановить отражение после смены CG.
 * Функция объявлена и у Tsumamigui 3 (fn411), т.е. это не разница версий, а
 * пробел движка для обеих.
 *
 * Аргумент состояния движок не различает: deform хранится на САМОЙ ЧАСТИ (так
 * же его пишет сеттер, читает рендер и сохраняет parts/save.c), а не в
 * состоянии.
 */
int PE_GetPartsCGDeform(int parts_no, possibly_unused int state)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->sprite_deform : 0;
}

/*
 * `SetComponentReverseLR/TB` (v14, у Tsumamigui 3 не объявлены вовсе) —
 * НЕЗАВИСИМЫЕ флаги зеркалирования части по каждой оси. Отдельны от
 * `sprite_deform`: тот одним числом 0/1/2 обе оси сразу выразить не может, а
 * игра ставит их порознь (`parts::detail::CParts@ReverseLR::set` ←
 * `CSpriteParts@ReverseLR::set` ← `AdvStandImage@Reverse::set` — зеркалирование
 * стоячего спрайта персонажа в ADV-сцене). У обоих есть геттер, значит свойство
 * обязано храниться по-настоящему; рендер складывает их с `sprite_deform`.
 */
/*
 * Разворот ПОМЕНЯЛСЯ: у самой части достаточно попросить перерисовку (отражение
 * своей картинки считает рендер), а вот система координат ПОТОМКОВ стала другой —
 * их надо переставить.
 *
 * ★Пересчёт зовём ТОЛЬКО когда дети есть. Два пути расчёта позиции (здесь и в
 * `parts_update_component`) сходятся не во всём — например, поворот родителя
 * учитывает лишь первый, — поэтому лишний пересчёт у бездетной части мог бы
 * сдвинуть её там, где раньше в силе было значение второго пути. Для бездетных
 * частей (все стоячие спрайты ADV, где разворот работал и до этой правки) правка
 * обязана быть строго нулевой.
 */
static void parts_reverse_changed(struct parts *parts)
{
	if (!TAILQ_EMPTY(&parts->children))
		parts_reposition_family(parts);
	parts_dirty(parts);
}

void PE_SetComponentReverseLR(int parts_no, bool reverse)
{
	// XSYS4_REV_WATCH=1 — КТО просит зеркалирование. Нужно, чтобы отличить «игра
	// просит, а мы не рисуем» от «игра не просит вовсе» (спрайты врагов в бою: ассет
	// нарисован лицом вправо, оригинал разворачивает врагов влево).
	// ★Фильтра по номеру части здесь БЫТЬ НЕ ДОЛЖНО: части ADV-сцены лежат выше
	// 1e9, а боевые — нет, и прежний порог `parts_no >= 1000000000` делал ловушку
	// слепой ровно к тому случаю, ради которого её ставили (вывод «в бою не просят»
	// был артефактом фильтра). Стек печатаем у первых вызовов, дальше — одну строку.
	// ★Дедуп по паре «часть+значение»: ADV-сцена переставляет разворот одним и тем
	// же спрайтам десятки раз и съедает любой бюджет ДО боя (именно так прежний
	// замер и «не увидел» боевые вызовы).
	if (getenv("XSYS4_REV_WATCH")) {
		static int left = 400, stacks_left = 24;
		static int seen_no[2048];
		static int8_t seen_val[2048];
		static int nseen = 0;
		bool dup = false;
		for (int i = 0; i < nseen; i++) {
			if (seen_no[i] == parts_no && seen_val[i] == (int8_t)reverse) {
				dup = true;
				break;
			}
		}
		if (!dup && nseen < 2048) {
			seen_no[nseen] = parts_no;
			seen_val[nseen] = (int8_t)reverse;
			nseen++;
		}
		if (left > 0 && !dup) {
			left--;
			struct parts *p = parts_try_get(parts_no);
			int nchildren = 0;
			if (p) {
				struct parts *child;
				PARTS_FOREACH_CHILD(child, p)
					nchildren++;
			}
			NOTICE("REVWATCH ReverseLR часть %d <- %d (тип=%d детей=%d родитель=%d)",
			       parts_no, (int)reverse,
			       p ? (int)p->states[p->state].type : -1, nchildren,
			       (p && p->parent) ? p->parent->no : -1);
			if (stacks_left > 0) {
				stacks_left--;
				vm_stack_trace();
			}
		}
	}
	struct parts *parts = parts_get(parts_no);
	if (parts->reverse_lr == reverse)
		return;
	parts->reverse_lr = reverse;
	parts_reverse_changed(parts);
}

void PE_SetComponentReverseTB(int parts_no, bool reverse)
{
	struct parts *parts = parts_get(parts_no);
	if (parts->reverse_tb == reverse)
		return;
	parts->reverse_tb = reverse;
	parts_reverse_changed(parts);
}

bool PE_GetComponentReverseLR(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->reverse_lr : false;
}

bool PE_GetComponentReverseTB(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->reverse_tb : false;
}

bool PE_SetPartsCGSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_cg *cg = parts_get_cg(parts, state);
	parts_set_surface_area(parts, &cg->common, x, y, w, h);
	return true;
}

void PE_GetPartsCGSurfaceArea(int parts_no, int *x, int *y, int *w, int *h, int state)
{
	if (!parts_state_valid(--state))
		return;

	struct parts_cg *cg = parts_get_cg(parts_get(parts_no), state);
	*x = cg->common.surface_area.x;
	*y = cg->common.surface_area.y;
	*w = cg->common.surface_area.w;
	*h = cg->common.surface_area.h;
}

int PE_GetPartsCGNumber(int parts_no, int state)
{
	if (!parts_state_valid(--state)) {
		return false;
	}

	return parts_get_cg(parts_get(parts_no), state)->no;
}

static bool _parts_animation_set_cg(struct parts *parts, struct parts_animation *anim,
		int start_no, int nr_frames, int frame_time,
		struct cg *(*load_cg)(int no, void *data), void *data)
{
	int w = 0, h = 0;
	Texture *frames = xcalloc(nr_frames, sizeof(Texture));
	for (int i = 0; i < nr_frames; i++) {
		struct cg *cg = load_cg(start_no + i, data);
		if (!cg) {
			for (int j = 0; j < i; j++) {
				gfx_delete_texture(&frames[j]);
			}
			free(frames);
			return false;
		}
		gfx_init_texture_with_cg(&frames[i], cg);
		w = max(w, cg->metrics.w);
		h = max(h, cg->metrics.h);
		cg_free(cg);
	}

	parts_set_dims(parts, &anim->common, w, h);
	free(anim->frames);
	anim->start_no = start_no;
	anim->frames = frames;
	anim->nr_frames = nr_frames;
	anim->frame_time = frame_time;
	anim->elapsed = 0;
	anim->current_frame = 0;
	anim->common.texture = frames[0];
	return true;
}

static struct cg *load_loop_cg_by_index(int no, void *_)
{
	return asset_cg_load(no);
}

bool parts_animation_set_cg_by_index(struct parts *parts, struct parts_animation *anim,
		int cg_no, int nr_frames, int frame_time)
{
	return _parts_animation_set_cg(parts, anim, cg_no, nr_frames, frame_time,
			load_loop_cg_by_index, NULL);
}

bool PE_SetLoopCG_by_index(int parts_no, int cg_no, int nr_frames, int frame_time, int state)
{
	if (!parts_state_valid(--state))
		return false;
	if (nr_frames <= 0 || nr_frames > 10000) {
		WARNING("Invalid frame count: %d", nr_frames);
		return false;
	}

	struct parts *parts = parts_get(parts_no);
	struct parts_animation *anim = parts_get_animation(parts, state);
	return parts_animation_set_cg_by_index(parts, anim, cg_no, nr_frames, frame_time);
}

static struct cg *load_loop_cg_by_name(int no, void *data)
{
	int unused_no;
	struct string *cg_name = string_format((struct string*)data, (union vm_value){.i=no}, STRFMT_INT);
	struct cg *cg = asset_cg_load_by_name(cg_name->text, &unused_no);
	free_string(cg_name);
	return cg;
}

bool parts_animation_set_cg(struct parts *parts, struct parts_animation *anim,
		struct string *cg_name, int start_no, int nr_frames, int frame_time)
{
	bool r = _parts_animation_set_cg(parts, anim, start_no, nr_frames, frame_time,
			load_loop_cg_by_name, cg_name);
	if (r)
		anim->cg_name = string_dup(cg_name);
	return r;
}

bool PE_SetLoopCG(int parts_no, struct string *cg_name, int start_no, int nr_frames,
	int frame_time, int state)
{
	if (!parts_state_valid(--state))
		return false;
	if (nr_frames <= 0 || nr_frames > 10000) {
		WARNING("Invalid frame count: %d", nr_frames);
		return false;
	}

	struct parts *parts = parts_get(parts_no);
	struct parts_animation *anim = parts_get_animation(parts, state);
	return parts_animation_set_cg(parts, anim, cg_name, start_no, nr_frames, frame_time);
}

bool PE_SetLoopCGSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state)) {
		WARNING("Invalid parts state: %d", state);
		return false;
	}

	struct parts *parts = parts_get(parts_no);
	struct parts_animation *anim = parts_get_animation(parts, state);
	parts_set_surface_area(parts, &anim->common, x, y, w, h);
	return true;
}

static void _parts_set_gauge_cg(struct parts *parts, struct parts_gauge *g, struct cg *cg)
{
	gfx_init_texture_with_cg(&g->cg, cg);
	gfx_init_texture_with_cg(&g->common.texture, cg);

	gfx_init_texture_rgba(&g->common.texture, g->cg.w, g->cg.h, (SDL_Color){0,0,0,255});
	gfx_copy_with_alpha_map(&g->common.texture, 0, 0, &g->cg, 0, 0, g->cg.w, g->cg.h);

	parts_set_dims(parts, &g->common, g->cg.w, g->cg.h);

	parts_dirty(parts);
}

bool parts_gauge_set_cg(struct parts *parts, struct parts_gauge *g, struct string *cg_name)
{
	int cg_no;
	struct cg *cg = asset_cg_load_by_name(cg_name->text, &cg_no);
	if (!cg)
		return false;
	_parts_set_gauge_cg(parts, g, cg);
	g->cg_no = cg_no;
	return true;
}

bool parts_gauge_set_cg_by_index(struct parts *parts, struct parts_gauge *g, int cg_no)
{
	struct cg *cg = asset_cg_load(cg_no);
	if (!cg)
		return false;
	_parts_set_gauge_cg(parts, g, cg);
	g->cg_no = cg_no;
	return true;
}

bool PE_SetHGaugeCG(int parts_no, struct string *cg_name, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_hgauge(parts, state);
	return parts_gauge_set_cg(parts, g, cg_name);
}

bool PE_SetHGaugeCG_by_index(int parts_no, int cg_no, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_hgauge(parts, state);
	return parts_gauge_set_cg_by_index(parts, g, cg_no);
}

bool PE_SetVGaugeCG(int parts_no, struct string *cg_name, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_vgauge(parts, state);
	return parts_gauge_set_cg(parts, g, cg_name);
}

bool PE_SetVGaugeCG_by_index(int parts_no, int cg_no, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_vgauge(parts, state);
	return parts_gauge_set_cg_by_index(parts, g, cg_no);
}

void parts_hgauge_set_rate(struct parts *parts, struct parts_gauge *g, float rate)
{
	if (!g->common.texture.handle) {
		WARNING("HGauge texture uninitialized");
		return;
	}
	int pixels = rate * g->cg.w;
	if (g->reverse) {
		// Заполнение от ПРАВОГО края: видимой остаётся правая полоса шириной
		// pixels, гасим левую. Координаты источника и приёмника совпадают —
		// картинка не сдвигается, меняется только какая её часть видна.
		int x = g->cg.w - pixels;
		gfx_copy_with_alpha_map(&g->common.texture, x, 0, &g->cg, x, 0, pixels, g->cg.h);
		gfx_fill_amap(&g->common.texture, 0, 0, x, g->cg.h, 0);
	} else {
		gfx_copy_with_alpha_map(&g->common.texture, 0, 0, &g->cg, 0, 0, pixels, g->cg.h);
		gfx_fill_amap(&g->common.texture, pixels, 0, g->cg.w - pixels, g->cg.h, 0);
	}
	parts_dirty(parts);
	g->rate = rate;
}

void parts_vgauge_set_rate(struct parts *parts, struct parts_gauge *g, float rate)
{
	if (!g->common.texture.handle) {
		WARNING("VGauge texture uninitialized");
		return;
	}
	int pixels = rate * g->cg.h;
	if (g->reverse) {
		// Зеркально горизонтальной: обычная вертикальная шкала растёт СНИЗУ
		// (видима нижняя часть), обратная — сверху.
		int h = g->cg.h - pixels;
		gfx_copy_with_alpha_map(&g->common.texture, 0, 0, &g->cg, 0, 0, g->cg.w, h);
		gfx_fill_amap(&g->common.texture, 0, h, g->cg.w, pixels, 0);
	} else {
		gfx_copy_with_alpha_map(&g->common.texture, 0, pixels, &g->cg, 0, pixels, g->cg.w, g->cg.h - pixels);
		gfx_fill_amap(&g->common.texture, 0, 0, g->cg.w, pixels, 0);
	}
	parts_dirty(parts);
	g->rate = rate;
}

bool PE_SetHGaugeRate(int parts_no, float numerator, float denominator, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_hgauge(parts, state);
	g->numerator = numerator;
	g->denominator = denominator;
	parts_hgauge_set_rate(parts, g, numerator/denominator);
	return true;
}

bool PE_SetHGaugeRate_int(int parts_no, int numerator, int denominator, int state)
{
	return PE_SetHGaugeRate(parts_no, numerator, denominator, state);
}

bool PE_SetVGaugeRate(int parts_no, float numerator, float denominator, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_vgauge(parts, state);
	g->numerator = numerator;
	g->denominator = denominator;
	parts_vgauge_set_rate(parts, g, (float)numerator/(float)denominator);
	return true;
}

bool PE_SetVGaugeRate_int(int parts_no, int numerator, int denominator, int state)
{
	return PE_SetVGaugeRate(parts_no, numerator, denominator, state);
}

static const struct parts_gauge *gauge_for_get(int parts_no, int state, bool vert);

/*
 * `Reverse` у шкалы: заполнение идёт от ПРОТИВОПОЛОЖНОГО края (горизонтальная —
 * справа налево, вертикальная — сверху вниз). Пара сеттер/геттер на обе оси.
 * Пока функций не было, экран Squad у Dohna ронял движок на первой же полосе:
 * `CHGaugeParts@Reverse::set` ← `PlayerSummaryView@SetParam` ←
 * `PartyPlayerViewCollection@CreateView`, а незнакомая HLL-функция у нас фатальна.
 * Флаг применяется СРАЗУ: игра ставит его после того, как задала отношение, и без
 * перерисовки полоса осталась бы заполненной с прежнего края.
 */
void PE_SetHGaugeReverse(int parts_no, bool enable, int state)
{
	if (!parts_state_valid(--state))
		return;
	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_hgauge(parts, state);
	if (g->reverse == enable)
		return;
	g->reverse = enable;
	if (g->common.texture.handle)
		parts_hgauge_set_rate(parts, g, g->rate);
}

bool PE_IsHGaugeReverse(int parts_no, int state)
{
	const struct parts_gauge *g = gauge_for_get(parts_no, state, false);
	return g && g->reverse;
}

void PE_SetVGaugeReverse(int parts_no, bool enable, int state)
{
	if (!parts_state_valid(--state))
		return;
	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_vgauge(parts, state);
	if (g->reverse == enable)
		return;
	g->reverse = enable;
	if (g->common.texture.handle)
		parts_vgauge_set_rate(parts, g, g->rate);
}

bool PE_IsVGaugeReverse(int parts_no, int state)
{
	const struct parts_gauge *g = gauge_for_get(parts_no, state, true);
	return g && g->reverse;
}

/*
 * Геттеры числителя/знаменателя (`Parts_GetHGaugeNumerator` и соседи). Игра
 * пользуется ими как обычными свойствами: `RankGauge@Attach` → `WorkerParamView`
 * читает числитель, чтобы поставить новый знаменатель, — без них экран подбора
 * работников Dohna падал на «Unimplemented HLL function».
 * Знаменатель по умолчанию 1: гейдж, которому отношение ещё не задавали, обязан
 * вести себя как «0 из 1», а не делить на ноль.
 */
static const struct parts_gauge *gauge_for_get(int parts_no, int state, bool vert)
{
	if (!parts_state_valid(--state))
		return NULL;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return NULL;
	// Смотрим, а НЕ заводим: `parts_get_hgauge`/`vgauge` при несовпадении типа
	// СБРАСЫВАЮТ состояние в гейдж, то есть геттер молча переписал бы часть,
	// которая гейджем не является. Не нашли — вернём NULL, вызывающий отдаст
	// дефолты (0 и 1).
	enum parts_type want = vert ? PARTS_VGAUGE : PARTS_HGAUGE;
	if (parts->states[state].type != want)
		return NULL;
	return &parts->states[state].gauge;
}

float PE_GetHGaugeNumerator(int parts_no, int state)
{
	const struct parts_gauge *g = gauge_for_get(parts_no, state, false);
	return g ? g->numerator : 0.0f;
}

float PE_GetHGaugeDenominator(int parts_no, int state)
{
	const struct parts_gauge *g = gauge_for_get(parts_no, state, false);
	return (g && g->denominator != 0.0f) ? g->denominator : 1.0f;
}

float PE_GetVGaugeNumerator(int parts_no, int state)
{
	const struct parts_gauge *g = gauge_for_get(parts_no, state, true);
	return g ? g->numerator : 0.0f;
}

float PE_GetVGaugeDenominator(int parts_no, int state)
{
	const struct parts_gauge *g = gauge_for_get(parts_no, state, true);
	return (g && g->denominator != 0.0f) ? g->denominator : 1.0f;
}

bool PE_SetHGaugeSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_hgauge(parts, state);
	parts_set_surface_area(parts, &g->common, x, y, w, h);
	return true;
}

bool PE_SetVGaugeSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_vgauge(parts, state);
	parts_set_surface_area(parts, &g->common, x, y, w, h);
	return true;
}

bool PE_SetNumeralCG(int parts_no, struct string *cg_name, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts_numeral *n = parts_get_numeral(parts_get(parts_no), state);
	n->font_no = parts_load_numeral_font_separate_string(cg_name);
	return true;
}

bool PE_SetNumeralCG_by_index(int parts_no, int cg_no, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_numeral *n = parts_get_numeral(parts_get(parts_no), state);
	n->font_no = parts_load_numeral_font_separate(cg_no);
	return true;
}

bool PE_SetNumeralLinkedCGNumberWidthWidthList_by_index(int parts_no, int cg_no,
		int w0, int w1, int w2, int w3, int w4, int w5, int w6, int w7, int w8,
		int w9, int w_minus, int w_comma, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct cg *cg = asset_cg_load(cg_no);
	if (!cg)
		return false;

	int w[12] = { w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w_minus, w_comma };
	struct parts_numeral *n = parts_get_numeral(parts_get(parts_no), state);
	n->font_no = parts_load_numeral_font_combined(cg, cg_no, w);
	return true;
}

bool PE_SetNumeralLinkedCGNumberWidthWidthList(int parts_no, struct string *cg_name,
		int w0, int w1, int w2, int w3, int w4, int w5, int w6, int w7, int w8,
		int w9, int w_minus, int w_comma, int state)
{
	if (!parts_state_valid(--state))
		return false;

	int no;
	struct cg *cg = asset_cg_load_by_name(cg_name->text, &no);
	if (!cg)
		return false;

	int w[12] = { w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w_minus, w_comma };
	struct parts_numeral *n = parts_get_numeral(parts_get(parts_no), state);
	n->font_no = parts_load_numeral_font_combined(cg, no, w);
	return true;
}

/*
 * `XSYS4_NUM_TRACE=1|<подстрока имени узла>` — КТО и ЧТО кладёт в числовые части.
 *
 * Зачем: счётчики интерфейса Dohna игра анимирует своим motion-движком
 * (`Motion::Create("… |Number:<конец>")` → `Motion::Executer@Update` →
 * `AFL_Parts_WrapNumeral` → `CNumeralParts@Number::set`), а движку достаются
 * только `Set/GetNumeralNumber`. Когда счётчик на экране стоит на нуле, по кадру
 * не отличить «игра не пишет» от «пишем, но не рисуется»: строка трассы называет
 * узел раскладки, значение и состояние. Живой случай — `Total Take` на экране
 * наград hustling (FINDINGS §5dz).
 *
 * Форма с подстрокой (`XSYS4_NUM_TRACE=Total`) отсекает шум: за анимацию в 500 мс
 * счётчик получает несколько десятков записей, а на экране их несколько.
 */
static bool num_trace_match(int parts_no, const char **name_out)
{
	static const char *tr = (const char *)1;
	if (tr == (const char *)1)
		tr = getenv("XSYS4_NUM_TRACE");
	if (!tr || !*tr)
		return false;
	const char *name = pe_parts_node_name(parts_no);
	*name_out = name ? name : "?";
	if (tr[0] == '1' && !tr[1])
		return true;
	return name && strstr(name, tr);
}

bool PE_SetNumeralNumber(int parts_no, int n, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_numeral *numeral = parts_get_numeral(parts, state);
	const char *name;
	if (num_trace_match(parts_no, &name))
		NOTICE("NUM set part=%d '%s' state=%d %d -> %d (act='%s')",
		       parts_no, name, state + 1, numeral->num, n,
		       pe_parts_activity_name(parts_no) ?: "-");
	return parts_numeral_set_number(parts, numeral, n);
}

/*
 * Геттеры числовой части. `Parts_GetNumeralNumber` обязателен не «для полноты»:
 * motion-движок читает ТЕКУЩЕЕ значение как начало интерполяции
 * (`Motion::Executer@GetValue<int>` ← `CNumeralParts@Number::get`), поэтому без
 * него первая же анимация счётчика денег на домашнем экране уходила в REPL.
 */
int PE_GetNumeralNumber(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	struct parts *parts = parts_try_get(parts_no);
	bool numeral = parts && parts->states[state].type == PARTS_NUMERAL;
	int r = numeral ? parts->states[state].num.num : 0;
	// Читается ОДИН раз на анимацию (начало интерполяции), поэтому печатаем каждое
	// чтение: важен и случай «часть не числовая» — тогда motion стартует с нуля, а
	// причина молчаливая.
	const char *name;
	if (num_trace_match(parts_no, &name))
		NOTICE("NUM get part=%d '%s' state=%d -> %d%s", parts_no, name,
		       state + 1, r, numeral ? "" : " (СОСТОЯНИЕ НЕ ЧИСЛОВОЕ)");
	return r;
}

bool PE_IsNumeralShowComma(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_NUMERAL)
		return false;
	return parts->states[state].num.show_comma;
}

int PE_GetNumeralSpace(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_NUMERAL)
		return 0;
	return parts->states[state].num.space;
}

int PE_GetNumeralLength(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_NUMERAL)
		return 0;
	return parts->states[state].num.length;
}

bool PE_SetNumeralShowComma(int parts_no, bool show_comma, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_numeral *num = parts_get_numeral(parts, state);
	if (num->show_comma == show_comma)
		return true;

	num->show_comma = show_comma;
	parts_numeral_update(parts, num);
	return true;
}

/*
 * `SetNumeralFont(no, type, size, r,g,b, boldWeight, edgeR,edgeG,edgeB,
 * edgeWeight, state)` — параметры ШРИФТОВОГО режима числовой части (см.
 * parts_numeral_font_update). Игра зовёт её сама для каждого счётчика; в
 * раскладке те же значения лежат полями `フォント*` рядом с `表示タイプ = 2`.
 * Установка параметров включает режим: CG-набор цифр и шрифт исключают друг
 * друга, и `ＣＧ名` у таких частей пуст.
 */
void PE_SetNumeralFont(int parts_no, int type, int size, int r, int g, int b,
		float bold_weight, int edge_r, int edge_g, int edge_b,
		float edge_weight, int state)
{
	if (!parts_state_valid(--state))
		return;
	struct parts *parts = parts_get(parts_no);
	struct parts_numeral *num = parts_get_numeral(parts, state);
	num->use_font = true;
	num->ts = default_text_style;
	num->ts.face = type;
	num->ts.size = size;
	num->ts.color = (SDL_Color) { r, g, b, 255 };
	num->ts.weight = bold_weight * 1000;
	num->ts.edge_color = (SDL_Color) { edge_r, edge_g, edge_b, 255 };
	text_style_set_edge_width(&num->ts, edge_weight);
	parts_numeral_update(parts, num);
	parts_dirty(parts);
}

/*
 * `SetNumeralShowType(no, type, state)`: 2 — рисовать шрифтом, иначе набором CG.
 * Значение приходит и из раскладки (`表示タイプ`). Другие значения, кроме 0/1/2,
 * не встречались — на них WARNING, чтобы не превратить неизвестный режим в
 * молчаливый CG.
 */
void PE_SetNumeralShowType(int parts_no, int type, int state)
{
	if (!parts_state_valid(--state))
		return;
	struct parts *parts = parts_get(parts_no);
	struct parts_numeral *num = parts_get_numeral(parts, state);
	if (type > 2) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("SetNumeralShowType: неизвестный режим %d (часть %d)", type, parts_no);
		}
	}
	num->use_font = (type == 2);
	parts_numeral_update(parts, num);
	parts_dirty(parts);
}

bool PE_SetNumeralSpace(int parts_no, int space, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_numeral *num = parts_get_numeral(parts, state);
	if (num->space == space)
		return true;

	num->space = space;
	parts_numeral_update(parts, num);
	return true;
}

bool PE_SetNumeralLength(int parts_no, int length, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_numeral *num = parts_get_numeral(parts, state);
	if (num->length == length)
		return true;

	num->length = max(1, length);
	num->have_num = true;
	parts_numeral_update(parts, num);
	return true;
}

bool PE_SetNumeralSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_numeral *n = parts_get_numeral(parts, state);
	parts_set_surface_area(parts, &n->common, x, y, w, h);
	return true;
}

/*
 * `XSYS4_RELEASE_TRACE=1` — КАЖДЫЙ игровой `ReleaseParts` со стеком, а не только по
 * наблюдаемым частям. Это ЕДИНСТВЕННЫЙ момент обнуления, видимый движку: игровой
 * `CParts@Release` зовёт нас и СРАЗУ ПОСЛЕ обнуляет своё поле `<Number>`
 * (`struct parts::detail::CParts { array<int> <vtable>; int <Number>; bool <AutoRelease>; }`),
 * а именно нулевой номер роняет `assert(parts.IsValid)` в `Motion::Create` (§5dc/§5dd).
 * Наблюдение по номеру тут не годится: номера меняются между прогонами, а виновная
 * часть заранее неизвестна — нужен весь поток.
 *
 * ★Печатаем и номер, и ЖИВА ЛИ часть: снятие уже снесённой части — отдельный случай,
 * который по одному номеру не отличить.
 */
void PE_ReleaseParts(int parts_no)
{
	if (parts_no == cg_watch_part) {
		NOTICE("CGWATCH part=%d ReleaseParts — стек вызовов игры:", parts_no);
		vm_stack_trace();
	}
	if (getenv("XSYS4_RELEASE_TRACE")) {
		NOTICE("RELEASE игра просит снять часть %d (жива=%d) — стек:",
		       parts_no, parts_try_get(parts_no) ? 1 : 0);
		vm_stack_trace();
	}
	parts_release(parts_no);
}

void PE_ReleaseAllParts(void)
{
	parts_release_all();
}

void PE_ReleaseAllPartsWithoutSystem(void)
{
	// FIXME: what's the difference?
	parts_release_all();
}

void PE_ReleaseAllWithoutSystem(struct page **erase_number_list)
{
	// Release all parts not belonging to the system overlay controller
	struct parts *parts = TAILQ_FIRST(&parts_list);
	while (parts) {
		struct parts *next = TAILQ_NEXT(parts, parts_list_entry);
		if (parts->controller_no != PARTS_CONTROLLER_SYSTEM_OVERLAY) {
			*erase_number_list = array_pushback(*erase_number_list,
					(union vm_value){.i = parts->no}, AIN_ARRAY_INT, -1);
			parts_release(parts->no);
		}
		parts = next;
	}

	// Drop all normal controllers and add a fresh default one.
	ctrl_stack.nr_controllers = 0;
	memset(ctrl_stack.hidden, 0, sizeof(ctrl_stack.hidden));
	PE_AddController(-1);
}

void PE_SetPos(int parts_no, int x, int y)
{
	parts_set_pos(parts_get(parts_no), (Point){ x, y });
}

void PE_SetZ(int parts_no, int z)
{
	// Под XSYS4_PARTS_TRACE: без этого не видно, кто и когда переставляет порядок
	// (у Dohna `Tutorial::SetPartsZandClipper` опускает корни сцен, чтобы поверх них
	// легла полосатая подложка обучения).
	if (getenv("XSYS4_PARTS_TRACE"))
		NOTICE("PARTS SetZ(%d, %d)", parts_no, z);
	parts_set_z(parts_get(parts_no), z);
}

// Highest Z currently in use (games query this to stack a new component/dialog
// on top of everything else). We ignore the component argument and report the
// maximum Z across all parts.
int PE_GetComponentAbsoluteMaxPosZ(int comp)
{
	(void)comp;
	int maxz = 0;
	struct parts *p;
	PARTS_LIST_FOREACH(p) {
		if (p->global.z > maxz)
			maxz = p->global.z;
	}
	return maxz;
}

void PE_SetShow(int parts_no, bool show)
{
	// Одно условие на обе метки наблюдения: parts_watched уже учитывает и
	// XSYS4_PART_WATCH (по номеру), и XSYS4_CG_WATCH (по картинке).
	if (getenv("XSYS4_PARTS_TRACE") || parts_watched(parts_no))
		NOTICE("PARTS SetShow(%d, %d)", parts_no, show);
	/*
	 * ★«ПОКАЗАТЬ» — это и есть обращение игры к части: если её слой снесён, а
	 * сама она жива (часть живой активности, §5ev), вернуть её на актуальный
	 * слой надо ЗДЕСЬ, а не ждать окна реплик. Иначе часть, переиспользуемая
	 * через обычный Show (а не через MW-путь), остаётся невидимой навсегда — и
	 * молча: игра считает её показанной, движок не рисует.
	 */
	void parts_adopt_to_active_layer(int parts_no);
	if (show)
		parts_adopt_to_active_layer(parts_no);
	parts_set_show(parts_get(parts_no), show);
}

void PE_SetAlpha(int parts_no, int alpha)
{
	/*
	 * XSYS4_ALPHA_TRACE=<номер части | 1> — кто и какую альфу ставит части.
	 * Нужна там, где альфу выставляют ДВЕ системы разом: подсветка боя Dohna
	 * просит силуэту 180 (`PlayerFrameView@GetAlpha(3)`), а покадровая анимация
	 * действия ставит слоям своё значение — если она перебивает корень, силуэт
	 * выходит непрозрачно-розовым вместо фиолетового (жалоба: у ВРАГА закрас
	 * срабатывает, у героя нет — у героя в это время играет анимация).
	 * `=1` — печатать все части, иначе только указанную.
	 */
	{
		static const char *w = (const char *)1;
		if (w == (const char *)1)
			w = getenv("XSYS4_ALPHA_TRACE");
		if (w && *w) {
			// `>=<номер>` — компактно (время + часть + альфа) для частей от номера;
			// иначе прежнее поведение: стек игры для указанной части.
			if (w[0] == '>' && w[1] == '=') {
				if (parts_no >= atoi(w + 2))
					NOTICE("SETALPHA t=%u часть %d <- %d",
					       SDL_GetTicks(), parts_no, alpha);
			} else {
				int only = atoi(w);
				if (only <= 1 || only == parts_no) {
					NOTICE("SETALPHA part=%d alpha=%d — стек игры:", parts_no, alpha);
					vm_stack_trace();
				}
			}
		}
	}
	parts_set_alpha(parts_get(parts_no), alpha);
}

void PE_SetPartsDrawFilter(int parts_no, int draw_filter)
{
	parts_get(parts_no)->draw_filter = draw_filter;
}

void PE_SetAddColor(int parts_no, int r, int g, int b)
{
	SDL_Color add_color = {
		min(255, max(0, r)),
		min(255, max(0, g)),
		min(255, max(0, b)),
		255
	};
	// XSYS4_MUL_TRACE=1 печатает и добавочный цвет: мерцание белым делается именно им,
	// и по кадру не отличить «игра не покрасила» от «покрасила, а не доехало».
	if (getenv("XSYS4_MUL_TRACE"))
		NOTICE("ADD part %d <- rgb %d,%d,%d", parts_no, r, g, b);
	/*
	 * XSYS4_COLOR_WATCH=1 — СТЕК ИГРЫ на первых непустых установках добавочного
	 * цвета. Отвечает на «кто это покрасил»: цвет ставят и подсветки, и анимации, и
	 * загрузчик раскладки, а по кадру и даже по значению автора не назвать. Печатаем
	 * ТОЛЬКО непустой цвет и ограниченно, иначе стек на каждый кадр забьёт лог.
	 */
	/*
	 * XSYS4_ADDCOLOR_TRACE=<r>,<g>,<b> — компактная строка (без стека, без лимита)
	 * при установке ИМЕННО этого цвета. Нужна, когда цвет ставится редко, а до
	 * интересного экрана запас печати вотча выше уже израсходован: так ищется
	 * АДРЕСАТ закраса боя Dohna (add = 255,0,186 у состояния `Exclude`).
	 */
	{
		static const char *w = (const char *)1;
		if (w == (const char *)1)
			w = getenv("XSYS4_ADDCOLOR_TRACE");
		if (w && *w) {
			// Форма `<r>,<g>,<b>` — только этот цвет; форма `>=<номер>` — ЛЮБОЙ
			// цвет частям от указанного номера (так видно и постановку закраса,
			// и то, чем его перебили, и через сколько миллисекунд).
			int wr = -1, wg = -1, wb = -1, min_no = 0;
			bool hit;
			if (w[0] == '>' && w[1] == '=') {
				min_no = atoi(w + 2);
				hit = parts_no >= min_no;
			} else {
				sscanf(w, "%d,%d,%d", &wr, &wg, &wb);
				hit = (r == wr && g == wg && b == wb);
			}
			if (hit)
				NOTICE("ADDCOLOR t=%u часть %d <- %d,%d,%d",
				       SDL_GetTicks(), parts_no, r, g, b);
		}
	}
	if ((r || g || b) && getenv("XSYS4_COLOR_WATCH")) {
		static int left = 24;
		// ★Фильтр по МИНИМАЛЬНОМУ номеру части: интересные части (вьюшки боя) имеют
		// номера от 1000000000, а UI-подсветки титула/меню — 90000xxx, и без фильтра
		// весь запас печати уходит на них ещё до нужного экрана.
		static int min_no = -1;
		if (min_no < 0)
			min_no = atoi(getenv("XSYS4_COLOR_WATCH"));
		if (left > 0 && parts_no >= min_no) {
			left--;
			NOTICE("COLORWATCH ADD часть %d <- %d,%d,%d — стек вызовов игры:",
			       parts_no, r, g, b);
			vm_stack_trace();
		}
	}
	parts_set_add_color(parts_get(parts_no), add_color);
}

/*
 * `減算色モード` — добавочный цвет ВЫЧИТАЕТСЯ вместо прибавления (см. sub_color_mode в
 * parts_internal.h). Пара была НАСТОЯЩЕЙ ДЫРОЙ: `SetComponentSubColorMode` не
 * экспортировалась, а незнакомая HLL-функция фатальна (`hll_call` → VM_ERROR) — экран
 * просмотра CG уходил в отладочный REPL прямо при построении миниатюр
 * (`CgThumbnailButton@RegisterStateEvent` → `CLayoutBoxParts@SubColorMode::set`), и
 * снаружи это выглядело как «экран открылся, ничего не нажимается».
 *
 * У сеттера ЕСТЬ геттер (`IsComponentSubColorMode`), поэтому значение обязано
 * храниться по-настоящему: no-op был бы отличим.
 */
void PE_SetSubColorMode(int parts_no, bool enable)
{
	if (getenv("XSYS4_MUL_TRACE"))
		NOTICE("SUBCOLOR part %d <- %d", parts_no, (int)enable);
	parts_get(parts_no)->sub_color_mode = enable;
}

bool PE_IsSubColorMode(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->sub_color_mode : false;
}

void PE_SetMultiplyColor(int parts_no, int r, int g, int b)
{
	SDL_Color multiply_color = {
		min(255, max(0, r)),
		min(255, max(0, g)),
		min(255, max(0, b)),
		255
	};
	if (getenv("XSYS4_MUL_TRACE"))
		NOTICE("MUL part %d <- rgb %d,%d,%d", parts_no, r, g, b);
	parts_set_multiply_color(parts_get(parts_no), multiply_color);
}

int PE_GetPartsX(int parts_no)
{
	return parts_get(parts_no)->local.pos.x;
}

int PE_GetPartsY(int parts_no)
{
	return parts_get(parts_no)->local.pos.y;
}

int PE_GetPartsWidth(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	return parts_get(parts_no)->states[state].common.w;
}

int PE_GetPartsHeight(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	return parts_get(parts_no)->states[state].common.h;
}

int PE_GetPartsUpperLeftPosX(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	struct parts *parts = parts_get(parts_no);
	int x = parts->states[state].common.hitbox.x;
	if (parts->parent)
		x += parts->parent->global.pos.x;
	return x;
}

int PE_GetPartsUpperLeftPosY(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	struct parts *parts = parts_get(parts_no);
	int y = parts->states[state].common.hitbox.y;
	if (parts->parent)
		y += parts->parent->global.pos.y;
	return y;
}

int PE_GetPartsZ(int parts_no)
{
	return parts_get(parts_no)->local.z;
}

bool PE_GetPartsShow(int parts_no)
{
	return parts_get(parts_no)->local.show;
}

int PE_GetPartsAlpha(int parts_no)
{
	return parts_get(parts_no)->local.alpha;
}

int PE_GetPartsDrawFilter(int parts_no)
{
	return parts_get(parts_no)->draw_filter;
}

void PE_GetAddColor(int parts_no, int *r, int *g, int *b)
{
	struct parts *parts = parts_get(parts_no);
	*r = parts->local.add_color.r;
	*g = parts->local.add_color.g;
	*b = parts->local.add_color.b;
}

void PE_GetMultiplyColor(int parts_no, int *r, int *g, int *b)
{
	struct parts *parts = parts_get(parts_no);
	*r = parts->local.multiply_color.r;
	*g = parts->local.multiply_color.g;
	*b = parts->local.multiply_color.b;
}

void PE_SetPartsOriginPosMode(int parts_no, int origin_pos_mode)
{
	struct parts *parts = parts_get(parts_no);
	parts_set_origin_mode(parts, origin_pos_mode);
	parts_dirty(parts);
}

int PE_GetPartsOriginPosMode(int parts_no)
{
	return parts_get(parts_no)->origin_mode;
}

void PE_SetParentPartsNumber(int parts_no, int parent_parts_no)
{
	struct parts *parts = parts_get(parts_no);
	if (getenv("XSYS4_PARTS_TRACE") || parts_watched(parts_no) || parts_watched(parent_parts_no))
		NOTICE("PARTS SetParentPartsNumber(%d, %d) parent_exists=%d",
		       parts_no, parent_parts_no,
		       parent_parts_no >= 0 ? parts_exists(parent_parts_no) : -1);
	/*
	 * ★РОДИТЕЛЕМ МОЖЕТ БЫТЬ СЛОЙ, а не часть. «Компонент» у AliceSoft — это и парт,
	 * и слой (та же двойственность, что у `SetComponentShow`, см.
	 * `parts_controller_is_layer`): назначить части родителем ID слоя значит
	 * «перенести её в этот слой», а не «сделать ребёнком части с таким номером».
	 *
	 * Живой случай — баннер уведомления Haha Ranman («データをセーブしました»).
	 * `wholeinfo::CParts@0` берёт `GetSystemOverlayLayer` и кладёт результат в
	 * `CParts@Parent::set` (замер `XSYS4_FN_TRACE_NS`: `a0=10000`, то есть
	 * PARTS_CONTROLLER_SYSTEM_OVERLAY). Пока это считалось номером части, баннер
	 * оставался в слое, где был создан (у Haha Ranman — слой 0, ADV), и при
	 * сохранении оказывался ПОД экраном SAVE: уведомление не показывалось вовсе.
	 * После загрузки, когда экран уже закрыт, тот же баннер было видно — отсюда
	 * и разница между сохранением и загрузкой.
	 *
	 * Дети переносить не нужно: слой наследуется от родителя при отрисовке.
	 * Откат для замеров: `XSYS4_NO_PARENT_AS_LAYER=1`.
	 */
	if (parent_parts_no > 0 && !parts_exists(parent_parts_no)
			&& (parent_parts_no == PARTS_CONTROLLER_SYSTEM_OVERLAY
				|| ctrl_stack_pos(parent_parts_no) >= 0)
			&& !getenv("XSYS4_NO_PARENT_AS_LAYER")) {
		if (getenv("XSYS4_PARTS_TRACE") || parts_watched(parts_no))
			NOTICE("PARTS   родитель %d — это СЛОЙ: часть %d перенесена в него",
			       parent_parts_no, parts_no);
		parts->controller_no = parent_parts_no;
		parts->pending_parent = -1;
		parts_component_dirty(parts);
		return;
	}
	/*
	 * ★НОЛЬ = ОТВЯЗАТЬ. `parts::detail::CParts@ClearParent` — это ровно
	 * `Parent::set(0)` (индекс 226 таблицы IParts), а библиотечная
	 * `Ｐ＿親解放(no)` = `Ｐ＿親設定(no, 0)`. Номер части 0 невалиден ни у одной
	 * игры (см. `PE_GetParentPartsNumber`), поэтому 0 и служит признаком снятия
	 * родителя — отдельного поля не нужно. Применяется отложенно, как и
	 * назначение: сама отвязка — в dirty-очереди `PE_UpdateComponent`.
	 *
	 * Слой при этом НЕ меняется: часть остаётся на том же экране, просто
	 * перестаёт быть чьим-то потомком.
	 *
	 * Внутренняя конвенция `-1` («изменений нет») — не путь наружу: раньше
	 * `PE_RemoveChild` передавал сюда -1, и снять родителя было нельзя ВООБЩЕ,
	 * помогал только `PE_ClearChild`, рвущий связь в обход очереди.
	 */
	parts->pending_parent = parent_parts_no;
	parts_component_dirty(parts);
}

/*
 * Перечисление ПОТОМКОВ (`NumofChild`/`GetChild`/`GetChildIndex`/`ClearChild`).
 * Апстрим отдавал 0/-1 заглушками, и на этом стоял ВЕСЬ обход дерева
 * активности: `activity::detail::CallUserComponentEventWithChild` (@0x29280)
 * спускается от «ルートパーツ» по `IParts@GetChild`, ищет части с типом
 * компонента 17 и создаёт для них экземпляры пользовательских компонентов.
 * Пустой список = ни одного созданного компонента = null у `GetUserComponent`.
 *
 * «Действующий» родитель — pending: назначение через PE_SetParentPartsNumber
 * откладывается до ближайшего UpdateComponent (dirty-очередь), а игра
 * спрашивает потомков СРАЗУ после загрузки раскладки, до апдейта.
 *
 * Порядок — обход общего списка частей (по z), а не TAILQ children: он один и
 * тот же для Numof/Get/GetChildIndex, чего требует их совместное употребление.
 */
static int parts_effective_parent_no(struct parts *p)
{
	if (p->pending_parent > 0)
		return p->pending_parent;
	// 0 — заказанная, но ещё не применённая ОТВЯЗКА (см. PE_SetParentPartsNumber):
	// для перечисления потомков парт уже ничей.
	if (p->pending_parent == 0)
		return -1;
	return p->parent ? p->parent->no : -1;
}

// «Убрать потомка» — отвязка, но только если он и правда потомок этого парта
// (`IsExistChild` у игры устроен так же: `GetParent(child) == parent`).
void PE_RemoveChild(int parts_no, int child_no)
{
	struct parts *child = parts_try_get(child_no);
	if (!child || parts_effective_parent_no(child) != parts_no)
		return;
	PE_SetParentPartsNumber(child_no, 0);
}

int PE_NumofChild(int parts_no)
{
	// «Дети парта -1» — это ВСЕ корневые парты: effective_parent сироты
	// тоже -1, и обход с невалидным номером зацикливал всё дерево.
	if (parts_no < 0)
		return 0;
	int n = 0;
	struct parts *p;
	PARTS_LIST_FOREACH(p) {
		if (parts_effective_parent_no(p) == parts_no)
			n++;
	}
	return n;
}

int PE_GetChild(int parts_no, int index)
{
	if (parts_no < 0 || index < 0)
		return -1;
	int n = 0;
	struct parts *p;
	PARTS_LIST_FOREACH(p) {
		if (parts_effective_parent_no(p) != parts_no)
			continue;
		if (n == index)
			return p->no;
		n++;
	}
	return -1;
}

int PE_GetChildIndex(int parts_no, int child_no)
{
	int n = 0;
	struct parts *p;
	PARTS_LIST_FOREACH(p) {
		if (parts_effective_parent_no(p) != parts_no)
			continue;
		if (p->no == child_no)
			return n;
		n++;
	}
	return -1;
}

// Отвязать ВСЕХ потомков. Через PE_SetParentPartsNumber(-1) этого не сделать:
// -1 в pending_parent означает «изменений нет», а не «нет родителя» (см. цикл
// dirty-очереди), поэтому связь рвём напрямую.
void PE_ClearChild(int parts_no)
{
	struct parts *parent = parts_try_get(parts_no);
	struct parts *p;
	PARTS_LIST_FOREACH(p) {
		if (parts_effective_parent_no(p) != parts_no)
			continue;
		if (p->pending_parent == parts_no)
			p->pending_parent = -1;
		if (parent && p->parent == parent) {
			TAILQ_REMOVE(&parent->children, p, child_list_entry);
			p->parent = NULL;
		}
		parts_component_dirty(p);
	}
}

int PE_GetParentPartsNumber(int parts_no)
{
	/*
	 * ЧИСТЫЙ ГЕТТЕР — части не СОЗДАЁТ (`parts_try_get`, не `parts_get`). Игра
	 * поднимается по цепочке родителей циклом «пока номер != 0» и на последнем
	 * витке спрашивает родителя у НЕСУЩЕСТВУЮЩЕГО номера — с `parts_get` этот
	 * вопрос заводил настоящую часть. Так у Haha Ranman заводилась ЧАСТЬ 0
	 * (`CMessageWindow@OnErasingLayer` → `Ｐ＿親設定取得`), она попадала в сейв,
	 * восстанавливалась при загрузке и снова уходила в следующий сейв.
	 */
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return 0;
	if (parts->parent)
		return parts->parent->no;
	// «Родителя нет» = 0, а НЕ -1. Внутри движка -1 (pending_parent, RemoveChild) —
	// своя конвенция, но НАРУЖУ games ждут 0, и это доказано байткодом обеих версий:
	//   Dohna v14 `AFL_Parts_GetLayerIDByParts` (@0x2b316c) идёт вверх по родителям
	//     циклом `while (no != 0) { p = GetParent(no); if (p == 0) return no; no = p; }`;
	//   Tsumamigui v7 `パーツ親メッセージウィンドウ設定取得` (@0x15fa34) —
	//     `if (GetParent(GetParent(x)) == 0) return 0`.
	// Номер части 0 не бывает валидным ни у одной из игр, поэтому 0 и есть «нет».
	// С прежним -1 цикл Dohna не завершался НИКОГДА: `-1 != 0`, дальше
	// `AFL_Parts_Wrap(-1)` (а parts_get(-1) ещё и СОЗДАВАЛ часть с номером -1),
	// её родитель снова -1 — вечный цикл на 100% CPU с аллокацией обёртки
	// CSpriteParts на каждом витке (RSS рос ~13 МБ/с, до 3.8 ГБ). Именно так
	// зависал уход с титула Dohna: клик по «Start Game» → SceneTitle@TitleClose →
	// EraseLayer → CallErasingLayerEvent → CModeMark@Init-лямбда → GetLayerIDByParts.
	/*
	 * ★У КОРНЕВОЙ ЧАСТИ РОДИТЕЛЬ — ЕЁ СЛОЙ. Цепочка родителей заканчивается не на
	 * корневом парте, а на НОМЕРЕ СЛОЯ, и уже у слоя родителя нет (0). Это та же
	 * двойственность «компонент = парт или слой», что и в `PE_SetParentPartsNumber`
	 * выше, только со стороны чтения — иначе get/set не были бы обратны друг другу.
	 *
	 * Доказательства байткодом Dohna (v14), все сайты-потребители `Ｐ＿親設定取得`:
	 *   • `AFL_Parts_GetLayerIDByParts` (@0x2b316c) и `ActivityInstances@GetLayer`
	 *     (FUNC 26625) — ОДИН и тот же подъём «пока parent != 0, вернуть последний
	 *     номер». Результат первого сравнивается с ID СТИРАЕМОГО СЛОЯ
	 *     (`CMessageWindow@OnErasingLayer(id)`, `CModeMark@Init`-лямбда) и уходит в
	 *     `モードマークレイヤ設定(layer)`; параметр второго так и зовётся
	 *     `layerPartsNumber` (`ActivityInstances@EraseByLayer`). Возвращая номер
	 *     КОРНЕВОГО ПАРТА, мы не совпадали с id слоя никогда: окно сообщений и
	 *     мод-марк не освобождались при стирании слоя.
	 *   • Round-trip get→set обязан сохранять размещение:
	 *     `CMessageWindow@Reload` запоминает `GetRootParts().Parent::get()`,
	 *     пересоздаёт окно и возвращает его тем же `Parent::set(...)`;
	 *     `Tutorial::MoveSceneParent` переносит подложку 下地 в контейнер сцены
	 *     через `bg.Core.Parent::set( sceneRoots[0].Parent::get() )`. С нулём
	 *     оба сайта теряли слой: подложка оставалась в АКТИВНОМ слое поверх сцены
	 *     и затемняла её (FINDINGS §5el, замер 46.9 против 70.9 у оригинала).
	 *   • `CInfoText@IsCreated::get` = `Core.Parent::get() != 0` — «созданная часть
	 *     всегда имеет ненулевого родителя», что верно только со слоем.
	 *   • `Ａ＿システムボタン設定` поднимается тем же циклом до парта окна сообщений и
	 *     ждёт 0 как «дошли до верха» — слой лишний виток даёт, но не ломает.
	 *
	 * Семантика ЕДИНА для всех версий, не только для Ixseal. В Tsumamigui (ain 7)
	 * `message::detail::CMessageWindowManager@GetPartsLayer` так и устроена: если
	 * список окон пуст — `AFL_Parts_GetActiveLayer()`, иначе `Ｐ＿親設定取得` от парта
	 * окна сообщений. Обе ветки обязаны вернуть СЛОЙ, то есть родитель корневого
	 * парта — его слой. Второй игровой потребитель там же (`CPartsPanel@
	 * BeginUpdateEvent`) — подъём «пока номер != 0», лишний виток через слой ему
	 * безразличен; остальные сайты Tsumamigui и Escalayer — редактор активностей,
	 * в игре не работающий. `AddController` объявлен в PartsEngine у всех четырёх
	 * игр (Dohna, Haha Ranman — ain 14; Tsumamigui — 7; Escalayer — 6.1), так что
	 * слои есть везде, и гейт по `parts_multi_controller` был бы фикцией.
	 *
	 * Самоссылка (часть с номером, равным номеру своего слоя) оборвала бы подъём в
	 * вечный цикл — отдаём 0.
	 */
	// Откат для замеров — та же ручка, что и у сеттера: `XSYS4_NO_PARENT_AS_LAYER=1`.
	if (parts->controller_no != parts_no && !getenv("XSYS4_NO_PARENT_AS_LAYER"))
		return parts->controller_no;
	return 0;
}

bool PE_SetPartsGroupNumber(possibly_unused int PartsNumber, possibly_unused int GroupNumber)
{
	UNIMPLEMENTED("(%d, %d)", PartsNumber, GroupNumber);
	return true;
}

void PE_SetPartsMessageWindowShowLink(possibly_unused int parts_no, bool message_window_show_link)
{
	struct parts *parts = parts_get(parts_no);
	parts->message_window = message_window_show_link;
}

bool PE_GetPartsMessageWindowShowLink(int parts_no)
{
	return parts_get(parts_no)->message_window;
}

void PE_SetPartsMagX(int parts_no, float scale_x)
{
	struct parts *parts = parts_get(parts_no);
	parts_set_scale_x(parts, scale_x);
}

float PE_GetPartsMagX(int parts_no)
{
	return parts_get(parts_no)->local.scale.x;
}

void PE_SetPartsMagY(int parts_no, float scale_y)
{
	struct parts *parts = parts_get(parts_no);
	parts_set_scale_y(parts, scale_y);
}

float PE_GetPartsMagY(int parts_no)
{
	return parts_get(parts_no)->local.scale.y;
}

/*
 * `XSYS4_ROT_TRACE=1` — КТО и на сколько разворачивает часть по X/Y, со стеком
 * игры (по одной строке на часть и ось). Нужен, чтобы понять СЕМАНТИКУ значения:
 * по логу Dohna в `RotateX` приходит 1000.0 (686 раз за прогон) вперемешку с
 * промежуточными 32.5/47.5/92.5/130 — то есть это анимация, но что за единицы,
 * из HLL-объявления (`void SetComponentRotateX(int, float)`) не видно.
 */
static void rot_trace(const char *axis, int parts_no, float v)
{
	static const char *tr = (const char *)1;
	if (tr == (const char *)1)
		tr = getenv("XSYS4_ROT_TRACE");
	if (!tr || !*tr)
		return;
	static struct { int no; char axis; } seen[128];
	static int nr_seen;
	for (int i = 0; i < nr_seen; i++)
		if (seen[i].no == parts_no && seen[i].axis == axis[0])
			return;
	if (nr_seen < 128) {
		seen[nr_seen].no = parts_no;
		seen[nr_seen].axis = axis[0];
		nr_seen++;
	}
	const char *name = pe_parts_node_name(parts_no);
	NOTICE("ROT%s part=%d '%s' <- %.3f (act='%s') — стек игры:", axis, parts_no,
	       name ? name : "?", v, pe_parts_activity_name(parts_no) ?: "-");
	vm_stack_trace();
}

/*
 * ★Предупреждение о 3D-развороте — ОДНОРАЗОВОЕ. Карточки-«двери» экрана наград
 * hustling получают `RotateY` КАЖДЫЙ КАДР на восьми частях: за прогон это десятки
 * тысяч строк, в которых тонет вся остальная диагностика (лог краша на итогах дня
 * вышел 74 МБ, и полезных строк в нём было полсотни). Смысл сообщения — сказать,
 * что разворот не реализован; повторы ничего не добавляют.
 */
void PE_SetPartsRotateX(int parts_no, float rot_x)
{
	if (rot_x != 0.0f)
		rot_trace("X", parts_no, rot_x);
	parts_set_rotation_x(parts_get(parts_no), rot_x);
}

void PE_SetPartsRotateY(int parts_no, float rot_y)
{
	if (rot_y != 0.0f)
		rot_trace("Y", parts_no, rot_y);
	parts_set_rotation_y(parts_get(parts_no), rot_y);
}

void PE_SetPartsRotateZ(int parts_no, float rot_z)
{
	parts_set_rotation_z(parts_get(parts_no), rot_z);
}

float PE_GetPartsRotateZ(int parts_no)
{
	return parts_get(parts_no)->local.rotation.z;
}

/*
 * Поворот по X/Y и номер части-альфа-клиппера читаются обратно.
 *
 * Сеттеры уже хранят и то и другое (`local.rotation.x/y`,
 * `alpha_clipper_parts_no`), геттеры же были `HLL_TODO_EXPORT` (`.fun = NULL`).
 * Нужны они не «для полноты»: motion-движок Ixseal читает ТЕКУЩЕЕ значение как
 * НАЧАЛО интерполяции (`CSpriteParts@<свойство>::get` ←
 * `Motion::Executer@GetValue<float>` ← `InitializeParams`), поэтому без геттера
 * первый же анимируемый поворот/клиппер валит движок в REPL. Ровно тот же
 * случай, что покомпонентные геттеры Add/Mul-цвета. Форма одинакова у v7 и v14.
 */
float PE_GetPartsRotateX(int parts_no)
{
	return parts_get(parts_no)->local.rotation.x;
}

float PE_GetPartsRotateY(int parts_no)
{
	return parts_get(parts_no)->local.rotation.y;
}

int PE_GetPartsAlphaClipperPartsNumber(int parts_no)
{
	return parts_get(parts_no)->alpha_clipper_parts_no;
}

void PE_SetPartsAlphaClipperPartsNumber(int parts_no, int alpha_clipper_parts_no)
{
	struct parts *parts = parts_get(parts_no);
	int old_no = parts->alpha_clipper_parts_no;
	if (old_no == alpha_clipper_parts_no)
		return;
	parts->alpha_clipper_parts_no = alpha_clipper_parts_no;

	/*
	 * Маску саму рисовать нельзя (см. parts_render), но и метка «я маска» не
	 * вечная: на одну маску ссылается несколько частей, поэтому считаем ссылки и
	 * снимаем метку, когда ушла последняя. Пока метка не снималась, часть,
	 * ПОБЫВАВШАЯ маской, не рисовалась уже никогда — так пропадал фон страниц
	 * CONFIG у Dohna: поверхность построена (`XSYS4_CP_DUMP` показывает размытый
	 * `背景／ナユタ`, затемнённый заливкой по альфе 180), часть показана
	 * (`gshow=1 alpha=255 tex=123`), а в дампе у неё `clip=0 isclip=1` — ссылок
	 * нет, метка осталась, и рендер её пропускал.
	 */
	if (old_no) {
		struct parts *old_clip = parts_try_get(old_no);
		if (old_clip && --old_clip->alpha_clipper_refs <= 0) {
			old_clip->alpha_clipper_refs = 0;
			old_clip->is_alpha_clipper = false;
			parts_dirty(old_clip);
		}
	}
	if (alpha_clipper_parts_no) {
		struct parts *clip = parts_try_get(alpha_clipper_parts_no);
		if (clip) {
			clip->alpha_clipper_refs++;
			clip->is_alpha_clipper = true;
			parts_dirty(clip);
		}
	}
	parts_dirty(parts);
}

void PE_SetPartsPixelDecide(int parts_no, bool pixel_decide)
{
	//UNIMPLEMENTED("(%d, %s)", parts_no, pixel_decide ? "true" : "false");
}

// ★Коэффициент уменьшения превью. Хранить его обязательно: он ЕДИНСТВЕННЫЙ признак,
// по которому различаются две несовместимые трактовки второго аргумента SaveThumbnail
// (см. PE_save_thumbnail). Tsumamigui 3 не зовёт эту функцию ВООБЩЕ (0 вхождений в
// байткоде), Escalayer Reboot зовёт перед каждым сохранением со значением 5.
static int thumbnail_reduction_size = 0;

bool PE_SetThumbnailReductionSize(int reduction_size)
{
	thumbnail_reduction_size = reduction_size;
	return true;
}

bool PE_SetThumbnailMode(bool mode)
{
	UNIMPLEMENTED("(%s)", mode ? "true" : "false");
	return true;
}

// ★ВТОРОЙ АРГУМЕНТ ЗНАЧИТ РАЗНОЕ У РАЗНЫХ ИГР, и это не догадка, а замер по двум играм:
//
//   Tsumamigui 3  — `SetThumbnailReductionSize` НЕ зовёт вовсе (0 вхождений в байткоде),
//                   параметр обёртки называется `ThumbnailWidth`, игра передаёт 200.
//                   Это ШИРИНА: превью выходят ~40 КБ и выглядят как надо.
//   Escalayer Reboot — зовёт `SetThumbnailReductionSize(5)` перед КАЖДЫМ сохранением
//                   (`AFL_GameSave_SaveThumbnail`: SetReductionSize → SetThumbnailMode(1)
//                   → Update → SaveThumbnail) и передаёт ту же пятёрку. Это ДЕЛИТЕЛЬ:
//                   трактовка «ширина» давала 5×3 px и файлы по 140 байт — в слотах
//                   сохранения картинок просто не было видно (нашёл пользователь).
//
// Различитель — сам факт вызова `SetThumbnailReductionSize`: у кого он задан, у того
// второй аргумент делитель. Подгонка по величине («меньше 32 — значит делитель») тут не
// нужна и была бы догадкой. Высота — по пропорции кадра.
bool PE_save_thumbnail(struct string *filename, int thumbnail_width)
{
	Texture *src = gfx_main_surface();
	int w;
	if (thumbnail_reduction_size > 1)
		w = src->w / thumbnail_reduction_size;
	else
		w = thumbnail_width > 0 ? thumbnail_width : src->w;
	if (w < 1)
		w = 1;
	if (w > src->w)
		w = src->w;
	int h = src->h * w / src->w;
	if (h < 1)
		h = 1;
	bool thtrace = getenv("XSYS4_TH_TRACE");
	if (thtrace) NOTICE("THUMB start: src=%dx%d width=%d -> %dx%d", src->w, src->h, thumbnail_width, w, h);

	// Downscale by repeatedly halving until we are within a factor of two of
	// the target size, then do the final stretch. This avoids the aliasing
	// that a single bilinear minification would otherwise produce.
	Texture tmp, *cur = src;
	bool have_tmp = false;
	while (cur->w / 2 > w && cur->h / 2 > h) {
		if (thtrace) NOTICE("THUMB halve: cur=%dx%d target=%dx%d", cur->w, cur->h, w, h);
		Texture next;
		gfx_init_texture_blank(&next, cur->w / 2, cur->h / 2);
		gfx_copy_stretch_with_alpha_map(&next, 0, 0, next.w, next.h, cur, 0, 0, cur->w, cur->h);
		if (have_tmp)
			gfx_delete_texture(&tmp);
		tmp = next;
		cur = &tmp;
		have_tmp = true;
	}

	Texture dst;
	gfx_init_texture_blank(&dst, w, h);
	gfx_copy_stretch_with_alpha_map(&dst, 0, 0, w, h, cur, 0, 0, cur->w, cur->h);
	if (have_tmp)
		gfx_delete_texture(&tmp);

	char *path = savedir_path(filename->text);
	if (thtrace) NOTICE("THUMB saving to %s", path);
	int r = gfx_save_texture(&dst, path, ALCG_QNT);
	if (thtrace) NOTICE("THUMB saved r=%d", r);
	free(path);
	gfx_delete_texture(&dst);
	return !!r;
}

void PE_SetInputState(int parts_no, int state)
{
	if (!parts_state_valid(--state)) {
		WARNING("invalid input state: %d", state);
		return;
	}
	parts_set_state(parts_get(parts_no), state);
}

int PE_GetInputState(int parts_no)
{
	return parts_get(parts_no)->state + 1;
}

/*
 * Область отсечения компонента (クリップ領域) — прямоугольник + флаг включения.
 * Все четыре ГЕТТЕРА в библиотеке есть (fn136-139) плюс IsComponentEnableClipArea
 * (fn134), т.е. игра читает значения обратно и no-op отличим: значения обязаны
 * храниться. Dohna анимирует их через motion — `Motion::Executer@SetPartsValue` →
 * `CSpriteParts@ClipWidth::set` → `CParts@ClipWidth::set` читает ClipX и пишет
 * ширину, так что без хранения ломается сама анимация, а не только вид.
 *
 * САМО отсечение при рендере пока НЕ применяется (у движка есть только
 * альфа-клиппер по части, прямоугольного scissor'а нет) — поэтому на включённую
 * непустую область один раз печатается WARNING, чтобы допущение было видно, а не
 * пряталось за тихим дефолтом.
 */
void PE_SetComponentEnableClipArea(int parts_no, bool enable)
{
	struct parts *parts = parts_get(parts_no);
	parts->clip_area_enabled = !!enable;
	if (enable && (parts->clip_area.w > 0 || parts->clip_area.h > 0)) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("PartsEngine: часть %d включила クリップ領域 (%d,%d %dx%d) — "
			        "значения хранятся, но отсечение при рендере не реализовано",
			        parts_no, parts->clip_area.x, parts->clip_area.y,
			        parts->clip_area.w, parts->clip_area.h);
		}
	}
}

bool PE_IsComponentEnableClipArea(int parts_no)
{
	return parts_get(parts_no)->clip_area_enabled;
}

void PE_SetComponentClipArea(int parts_no, int x, int y, int w, int h)
{
	struct parts *parts = parts_get(parts_no);
	parts->clip_area = (Rectangle) { .x = x, .y = y, .w = w, .h = h };
}

int PE_GetComponentClipAreaPosX(int parts_no)
{
	return parts_get(parts_no)->clip_area.x;
}

int PE_GetComponentClipAreaPosY(int parts_no)
{
	return parts_get(parts_no)->clip_area.y;
}

int PE_GetComponentClipAreaPosWidth(int parts_no)
{
	return parts_get(parts_no)->clip_area.w;
}

int PE_GetComponentClipAreaPosHeight(int parts_no)
{
	return parts_get(parts_no)->clip_area.h;
}

// System 4 v14 (Ixseal: Dohna Dohna, Healing Touch) extended the component-type
// enum: eight new UI widget types (メッセージウィンドウ, スピンボックス,
// 縦/横スライダーバー, パネル, フォーム, フォームグループ, ユーザコンポーネント)
// were inserted at ids 10-17, shifting the parts family (低レベルパーツ ..
// ムービーパーツ) from 10-22 up to 18-30. Ids 0-9 (button, checkbox, scrollbars,
// textbox, listbox, combobox, multiline textbox, layout box, radio button box)
// are unchanged. Verified against parts::detail::GetComponentTypeName in both
// dohnadohna.ain (v14) and Tsumamigui3.ain (v7).
#define COMPONENT_TYPE_SHIFT 8

static bool shifted_component_types(void)
{
	return instructions[CALLMETHOD].args[0] == T_INT;
}

// Translate a component type id from the game's enum to the classic numbering
// used by the switches below.
static int component_type_to_classic(int type)
{
	if (!shifted_component_types() || type < 10)
		return type;
	if (type < 10 + COMPONENT_TYPE_SHIFT)
		return -1;  // v14-only UI widget; no classic equivalent
	return type - COMPONENT_TYPE_SHIFT;
}

static int component_type_from_classic(int type)
{
	if (!shifted_component_types() || type < 10)
		return type;
	return type + COMPONENT_TYPE_SHIFT;
}

void PE_SetComponentType(int parts_no, int type, int state)
{
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("SetComponentType part=%d type=%d state=%d", parts_no, type, state);
	// Парта ещё нет — НЕ материализовать: игровые обёртки зовут это прямо из
	// конструктора, до реального создания (см. pending_ctype_table).
	if (!parts_try_get(parts_no)) {
		pending_ctype_set(parts_no, type, state);
		return;
	}
	if (!parts_state_valid(--state))
		return;
	struct parts *parts = parts_get(parts_no);
	enum parts_type pt = PARTS_UNINITIALIZED;
	// Виджеты, добавленные в v14 (id 10-17), классического аналога не имеют и
	// потому обрабатываются ДО перевода в классическую нумерацию. Из них движок
	// пока умеет только панель (14, `パネル`) — см. src/parts/panel.c.
	if (shifted_component_types() && type == 14) {
		if (parts->states[state].type != PARTS_PANEL)
			parts_state_reset(&parts->states[state], PARTS_PANEL);
		return;
	}
	// Место под чужую активность (`ユーザコンポーネント`, id 17): своего рендера
	// нет, поэтому это ФЛАГ на обычной части, как у кнопки, а не вид состояния.
	// Состояние не сбрасываем — потомков вешает сама игра.
	if (shifted_component_types() && type == 17) {
		parts->is_user_component = true;
		return;
	}
	// Кнопка (id 0 в обеих нумерациях): у движка это не отдельный вид рендера, а
	// ФЛАГ на CG-части (`is_button`), который и отдаёт обратно PE_GetComponentType,
	// — так же её помечает загрузчик раскладок. Состояние сбрасывать НЕЛЬЗЯ:
	// parts_state_reset затёр бы CG, уже загруженный из раскладки.
	// Ixseal конструирует кнопку сама: `parts::detail::CButtonParts@0` (@0x2f0aaa)
	// завершается вызовом `SetComponentType(no, 0, 1)`, и без этой ветки первая же
	// кнопка титула валила движок («unknown component type 0»). У v6/v7 тип 0 тоже
	// означал кнопку, но они его не выставляли — ветка ничего не меняет для них.
	if (type == 0) {
		parts->is_button = true;
		return;
	}
	// Чекбокс (id 1 в обеих нумерациях) — ровно тот же случай, что кнопка: у
	// движка это ФЛАГ на CG-части, а состояние сбрасывать нельзя (загрузчик
	// раскладок уже положил в него CG рамки, см. PE_InitPartsCheckBox).
	// Ixseal конструирует его сама: `parts::detail::CCheckBoxParts@0` (@0x2fc894)
	// завершается вызовом `SetComponentType(no, 1, 1)` — без этой ветки первый же
	// чекбокс валил движок («unknown component type 1»).
	if (type == 1) {
		parts->is_checkbox = true;
		return;
	}
	// Поле ввода (4) — тот же случай: рисуется своим кодом (таблица в
	// src/hll/PartsEngine.c), а вид компонента обязано отдавать своим, иначе обёртка
	// `CActivityWrap@GetTextBox` у игры пустая. Номер 4 совпадает в обеих нумерациях.
	if (type == 4) {
		parts->is_textbox = true;
		return;
	}
	// Группа радиокнопок (9) — тоже флаг на части без своего вида отрисовки.
	if (type == 9) {
		parts->is_radio_box = true;
		return;
	}
	// Полосы прокрутки (2/3) — тоже ФЛАГ на CG-части (геометрию кладёт загрузчик
	// раскладок, PE_InitPartsVScrollbar/HScrollbar). Игра выставляет тип сама:
	// `CVScrollBarParts@0` (@0x3c6b9c) → SetComponentType(no, 2, 1),
	// `CHScrollBarParts@0` (@0x32bd1e) → 3.
	if (type == 2 || type == 3) {
		if (type == 2)
			parts->is_vscrollbar = true;
		else
			parts->is_hscrollbar = true;
		return;
	}
	/*
	 * Слайдеры (`縦スライダーバー` = 12, `横スライダーバー` = 13 в нумерации v14) —
	 * тот же случай, что полосы прокрутки: ФЛАГ на CG-части, геометрию кладёт
	 * загрузчик раскладок. Игра выставляет тип сама (`CHSliderBarParts@0` →
	 * SetComponentType(no, 13, 1)); раньше это уходило в общую ветку виджетов и
	 * печатало «виджет 13 без своего вида отрисовки», а ползунок прозрачности
	 * оставался без бегунка. Гейт по нумерации обязателен: классические 12/13 —
	 * это `ループＣＧパーツ` и `テキストパーツ`.
	 */
	if (shifted_component_types() && (type == 12 || type == 13)) {
		if (type == 12)
			parts->is_vscrollbar = true;
		else
			parts->is_hscrollbar = true;
		parts->is_slider_bar = true;
		return;
	}
	switch (component_type_to_classic(type)) {
	case 8:  pt = PARTS_LAYOUT_BOX; break;
	case 11: pt = PARTS_CG; break;
	case 12: pt = PARTS_ANIMATION; break;
	case 13: pt = PARTS_TEXT; break;
	case 14: pt = PARTS_HGAUGE; break;
	case 15: pt = PARTS_VGAUGE; break;
	case 16: pt = PARTS_NUMERAL; break;
	case 17: pt = PARTS_RECT_DETECTION; break;
	case 18: pt = PARTS_CONSTRUCTION_PROCESS; break;
	case 19: pt = PARTS_CG_DETECTION; break;
	case 20: pt = PARTS_FLAT; break;
	case 21: pt = PARTS_3DLAYER; break;
	case 22: pt = PARTS_MOVIE; break;
	default: {
		/*
		 * ВИДЖЕТ, а не `パーツ`, и притом такой, которому движок не ведёт
		 * отдельного флага: слайдеры, поля ввода, списки, формы, окно
		 * сообщения. Своего вида отрисовки у них нет (окно сообщения, например,
		 * собрано из служебных частей-потомков), а тип состояния обязан
		 * остаться прежним. Те виджеты, у которых флаг есть, разобраны ВЫШЕ
		 * поимённо: кнопка (0), чекбокс (1), полосы прокрутки (2/3),
		 * `パネル` (14 в v14) и `ユーザコンポーネント` (17 в v14).
		 *
		 * Раньше здесь была VM_ERROR, и первый же `横スライダーバー` (13) на
		 * игровом экране Haha Ranman `行動選択` ронял движок в REPL. Сбрасывать
		 * состояние тоже нельзя: parts_state_reset затёр бы CG, уже загруженный
		 * раскладкой, — по той же причине этого не делает ветка кнопки.
		 *
		 * `パーツ`-семейство (классические id 11..22) сюда попасть не должно —
		 * там неизвестный тип по-прежнему ошибка.
		 */
		int classic = component_type_to_classic(type);
		if (classic >= 11)
			VM_ERROR("unknown component type %d", type);
		static bool warned[32];
		unsigned w = (unsigned)type < 32 ? (unsigned)type : 31;
		if (!warned[w]) {
			warned[w] = true;
			WARNING("SetComponentType: виджет %d без своего вида отрисовки — "
				"тип состояния оставлен как есть", type);
		}
		return;
	}
	}
	if (parts->states[state].type != pt)
		parts_state_reset(&parts->states[state], pt);
}

static int pe_get_component_type(int parts_no, int state);

// XSYS4_CTYPE_TRACE=1 — какой вид части движок отдаёт игре. Игра почти везде спрашивает
// его ПЕРЕД тем, как взять обёртку (`CActivityWrap@CompParts(имя, тип, состояние)`), и при
// несовпадении молча возвращает null — экран строится, а кусок работы не делается вовсе.
// Без этой трассы такие места видно только по последствиям.
/*
 * Пометить часть как ГРУППУ РАДИОКНОПОК. Зовёт загрузчик раскладки (パーツタイプ=9);
 * как и у поля ввода, парта на этот момент может ещё не быть — тогда заявка ложится в
 * таблицу отложенных типов.
 */
void parts_mark_radio_box(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	if (parts) {
		parts->is_radio_box = true;
		return;
	}
	pending_ctype_set(parts_no, 9, 1);
}

// Записать кнопку в состав группы (зовёт загрузчик раскладки, разрешив имя в номер).
void parts_radio_box_add_child(int box_no, int child_no)
{
	struct parts *box = parts_try_get(box_no);
	if (!box || child_no < 0)
		return;
	box->is_radio_box = true;
	for (int i = 0; i < box->nr_radio_children; i++)
		if (box->radio_children[i] == child_no)
			return;
	box->radio_children = xrealloc_array(box->radio_children, box->nr_radio_children,
			box->nr_radio_children + 1, sizeof(*box->radio_children));
	box->radio_children[box->nr_radio_children++] = child_no;
}

/*
 * Группа, которой принадлежит кнопка. В раскладке группа — ПУСТАЯ ЧАСТЬ-СОСЕД: и она, и
 * сами кнопки лежат детьми одного layout-бокса (замер по `コンフィグ／０６／ウィンドウ`:
 * бокс 90000075 → группа 90000076 + кнопки 90000077/90000079). Поэтому ищем среди
 * братьев, а не среди предков.
 */
static struct parts *parts_radio_box_of(struct parts *parts, int *idx_out)
{
	struct parts *box;
	PARTS_LIST_FOREACH(box) {
		if (!box->is_radio_box)
			continue;
		for (int i = 0; i < box->nr_radio_children; i++) {
			if (box->radio_children[i] != parts->no)
				continue;
			if (idx_out)
				*idx_out = i;
			return box;
		}
	}
	return NULL;
}

/*
 * Номер ГРУППЫ, которой принадлежит кнопка, и её индекс в группе; -1, если кнопка вне
 * группы. Нужно слою ввода: сообщение о выборе адресовано ГРУППЕ, а не кнопке.
 */
int parts_radio_box_number(int parts_no, int *index_out)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return -1;
	struct parts *box = parts_radio_box_of(parts, index_out);
	return box ? box->no : -1;
}

// Пометить часть как ПОЛЕ ВВОДА. Зовёт загрузчик раскладки (パーツタイプ=4): само
// состояние поля живёт в таблице src/hll/PartsEngine.c, а вид компонента отдаёт парт.
void parts_mark_textbox(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	if (parts) {
		parts->is_textbox = true;
		return;
	}
	// ★Часть на этот момент ЕЩЁ НЕ СОЗДАНА: загрузчик раскладки настраивает поле
	// ввода раньше, чем парт материализуется (замер: `parts_try_get` даёт NULL для
	// обоих полей `テキストボックス：コメント編集`). Кладём заявку в ту же таблицу
	// отложенных типов, что и SetComponentType, — она применится при создании.
	pending_ctype_set(parts_no, 4, 1);
}

/*
 * Пометить полосу как СЛАЙДЕР (`横スライダーバー` = 13, `縦スライダーバー` = 12 в
 * нумерации v14). Зовёт загрузчик раскладки: механику (жёлоб, бегунок, доля) ведут
 * те же поля, что у полосы прокрутки, а этот признак меняет только вид компонента,
 * который часть отдаёт игре, — иначе обёртка `GetHSliderBar` у игры пустая.
 */
void parts_mark_slider_bar(int parts_no, bool horizontal)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts) {
		pending_ctype_set(parts_no, horizontal ? 13 : 12, 1);
		return;
	}
	if (horizontal)
		parts->is_hscrollbar = true;
	else
		parts->is_vscrollbar = true;
	parts->is_slider_bar = true;
}

/*
 * Пометить часть как КОРЕНЬ АКТИВНОСТИ (`ルートパーツ` раскладки). Зовёт загрузчик
 * раскладки — единственный, кто это знает: в рантайме корень от любого другого
 * контейнера ничем не отличается. Признак нужен точке привязки, см.
 * parts_anchor_shift и FINDINGS §5dz.
 */
void parts_mark_activity_root(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	if (parts)
		parts->is_activity_root = true;
}

int PE_GetComponentType(int parts_no, int state)
{
	int r = pe_get_component_type(parts_no, state);
	if (getenv("XSYS4_CTYPE_TRACE"))
		NOTICE("CTYPE no=%d state=%d -> %d", parts_no, state, r);
	return r;
}

static int pe_get_component_type(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return -1;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts) {
		// Тип мог быть назначен до материализации парта (см. pending_ctype_table).
		struct pending_ctype *pc = pending_ctype_get(parts_no);
		if (pc && pc->state - 1 == state)
			return pc->type;
		/*
		 * ★ОТВЕРГНУТО ЗАМЕРОМ: «отдавать тип CG вместо -1, чтобы `parts::detail::Wrap`
		 * (он начинается с `GetComponentType`) не делал НЕВАЛИДНУЮ обёртку». Ассерт
		 * `Executer.jaf:55` у Dohna не уходит — значит валидность обёртки Wrap берёт
		 * не только отсюда. Не возвращать без нового замера.
		 */
		return -1;
	}

	// Чекбокс (パーツタイプ=1) тоже рисуется CG-частью, но обязан отвечать своим
	// типом: `CActivityWrap@GetCheckBox` (@0x200c8) отдаёт часть только если
	// `CompParts(имя, 1, 1)`, иначе возвращает null. Из-за отсутствия этой ветки
	// `SceneYesNoDialog@Close` читал `GetCheckBox("CheckDontShowAgain").Checked`
	// у null-интерфейса и падал на `X_REF` (диалог «Save the game?» → Yes).
	// Id 1 из v14-части перечисления — сдвиг его не касается (он с 18).
	// Проверяется РАНЬШЕ is_button: чекбокс кликабелен, но кнопкой не является.
	if (parts->is_checkbox)
		return 1;

	// Поле ввода (パーツタイプ=4) — та же двойственность: рисуется своим кодом, а вид
	// обязано отдавать своим, иначе `CActivityWrap@GetTextBox` (гейт `CompParts(имя, 4,
	// состояние)`) отдаёт игре null. Подробности — у флага в parts_internal.h.
	if (parts->is_textbox)
		return 4;

	// Группа радиокнопок (パーツタイプ=9) — то же самое ради
	// `CActivityWrap@GetRadioButtonBox` (`CompParts(имя, 9, состояние)`).
	if (parts->is_radio_box)
		return 9;

	// Полосы прокрутки (縦=2, 横=3) — тот же случай: рисуются CG-ползунком, а
	// тип обязаны отдавать свой, иначе `CActivityWrap@GetVScrollBar` (@0x202f0,
	// `CompParts(имя, 2, 1)`) и `GetHScrollBar` (@0x20500, тип 3) возвращают
	// null. На этом падал ассерт игры `ScrollBarUnit.jaf:26:
	// (nonnull) m_act.GetVScrollBar("Scroll")` при входе на экран «Items».
	// У СЛАЙДЕРА (`縦/横スライダーバー`) механика полосы, но свой вид: 12/13 в
	// нумерации v14. `CActivityWrap@GetHSliderBar` (FUNC 552) сверяет его через
	// `CompParts(имя, 13, 1)`, поэтому ответ «3» отдал бы игре пустую обёртку.
	if (parts->is_vscrollbar)
		return parts->is_slider_bar ? 12 : 2;
	if (parts->is_hscrollbar)
		return parts->is_slider_bar ? 13 : 3;

	// Activity "button" parts (パーツタイプ=0) render as CG but report type 0 so
	// the game recognizes them as buttons (e.g. C_TITLE@Enable registers click
	// handlers only on type-0 parts).
	if (parts->is_button)
		return 0;

	// Место под чужую активность: игра ищет его сравнением
	// `CActivityWrap@CompParts(имя, 17, 1)` (@0x1fd54), т.е. тип обязан быть
	// ровно 17 в нумерации v14 (сдвиг её не касается — он начинается с 18).
	if (parts->is_user_component)
		return 17;

	// Панель — виджет из v14-части перечисления: её id 14 НЕ сдвигается
	// (сдвиг касается только классического семейства パーツ, id 18+).
	if (parts->states[state].type == PARTS_PANEL)
		return 14;

	int classic;
	switch (parts->states[state].type) {
	case PARTS_LAYOUT_BOX: classic = 8; break;
	case PARTS_UNINITIALIZED:  // defaluts to CG
	case PARTS_CG:
		classic = 11;
		break;
	case PARTS_ANIMATION: classic = 12; break;
	case PARTS_TEXT: classic = 13; break;
	case PARTS_HGAUGE: classic = 14; break;
	case PARTS_VGAUGE: classic = 15; break;
	case PARTS_NUMERAL: classic = 16; break;
	case PARTS_RECT_DETECTION: classic = 17; break;
	case PARTS_CONSTRUCTION_PROCESS: classic = 18; break;
	case PARTS_CG_DETECTION: classic = 19; break;
	case PARTS_FLAT: classic = 20; break;
	case PARTS_3DLAYER: classic = 21; break;
	case PARTS_MOVIE: classic = 22; break;
	case PARTS_FLASH:
	default:
		VM_ERROR("unsupported component type %d", parts->states[state].type);
	}
	return component_type_from_classic(classic);
}

// Horizontal-scrollbar / slider. The part's CG is the knob; it slides along the
// track (length sb_length) between sb_base_x .. sb_base_x + (length - knob_w).
// Position the knob for the current rate (0..1). Called on init, on the game's
// per-frame SetHScrollbarScrollRate, and while dragging.
static void parts_hscrollbar_reposition(struct parts *parts)
{
	if (!parts->is_hscrollbar)
		return;
	struct parts_common *c = &parts->states[PARTS_STATE_DEFAULT].common;
	int knob_w = c->w, knob_h = c->h;
	float r = parts->hscroll_rate;
	if (r < 0.0f) r = 0.0f;
	if (r > 1.0f) r = 1.0f;
	int travel = parts->sb_length - knob_w;
	if (travel < 0) travel = 0;
	int x = parts->sb_base_x + (int)(r * travel + 0.5f);
	int y = parts->sb_base_y + (parts->sb_width - knob_h) / 2;
	parts_set_pos(parts, (Point){ x, y });
}

void PE_InitPartsHScrollbar(int parts_no, int base_x, int base_y,
		int length, int width, int total, int view, float rate)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return;
	parts->is_hscrollbar = true;
	parts->sb_base_x = base_x;
	parts->sb_base_y = base_y;
	parts->sb_length = length;
	parts->sb_width = width;
	parts->sb_total = total;
	parts->sb_view = view;
	parts->hscroll_rate = rate;
	parts_hscrollbar_reposition(parts);
}

// Drag the knob so its centre follows the cursor (absolute window x), clamped to
// the track; update the rate the game polls via GetHScrollbarScrollRate.
void parts_hscrollbar_drag_to(struct parts *parts, int cursor_abs_x)
{
	if (!parts->is_hscrollbar)
		return;
	struct parts_common *c = &parts->states[PARTS_STATE_DEFAULT].common;
	int knob_w = c->w;
	int travel = parts->sb_length - knob_w;
	if (travel <= 0)
		return;
	int parent_x = parts->parent ? parts->parent->global.pos.x : 0;
	int knob_left = cursor_abs_x - parent_x - parts->sb_base_x - knob_w / 2;
	float r = (float)knob_left / (float)travel;
	if (r < 0.0f) r = 0.0f;
	if (r > 1.0f) r = 1.0f;
	parts->hscroll_rate = r;
	if (getenv("XSYS4_SLIDER_TRACE"))
		NOTICE("SLIDER drag no=%d rate=%.3f", parts->no, r);
	parts_hscrollbar_reposition(parts);
}

/*
 * Протяжка горизонтальной полосы/слайдера ОБЪЯВЛЯЕТСЯ ИГРЕ — тем же событием прокрутки,
 * что и у вертикальной (`PE_OnVScrollbarDragged`, src/hll/PartsEngine.c). Обработчик у
 * игры один и тот же по форме: `DG_ScrollHandler(number, scrollPos, total)`.
 *
 * Отличие от вертикальной — ОТКУДА размеры: вертикальной их ведёт HLL-структура
 * `pe_vscrollbar` (игра задаёт Total/View вызовами), а горизонтальную целиком описывает
 * раскладка (`長さ/幅/全体スクロール量/表示量` → PE_InitPartsHScrollbar), поэтому Total/View
 * берём с самой части — и функция живёт здесь, где `struct parts` видна целиком.
 *
 * Обработчику конфига позиция не нужна (он перечитывает долю через
 * GetHSliderBarScrollRate), но передаём честную: полосам прокрутки она нужна.
 */
void PE_OnHScrollbarDragged(int parts_no, float rate)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return;
	int max = parts->sb_total - parts->sb_view;
	if (max < 0) max = 0;
	if (rate < 0.0f) rate = 0.0f;
	if (rate > 1.0f) rate = 1.0f;
	parts_msg_push(parts, PARTS_MSG_SCROLL, "ii",
			(int)(rate * max + 0.5f), parts->sb_total);
}

void PE_SetPartsHScrollbarScrollRate(int parts_no, float rate)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return;
	parts->hscroll_rate = rate;
	parts_hscrollbar_reposition(parts);
}

/*
 * `横スライダーバー` (тип 13) — ВИДЖЕТ, а не `パーツ`: своего вида отрисовки у него
 * нет, ползунок игра ведёт сама поверх обычной CG-части (см. PE_SetComponentType).
 * Из всего слайдер-API Haha Ranman зовёт только долю прокрутки, поэтому храним её
 * в том же поле, что у горизонтальной полосы прокрутки, и отдаём обратно —
 * остальные функции слайдера не выдумываем, пока их не позовут.
 *
 * Без этой пары игра валилась в debug-REPL, и запускалась лишь костылём
 * `XSYS4_LENIENT_HLL=1`.
 */
void PE_SetHSliderBarScrollRate(int parts_no, float rate)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return;
	if (rate < 0.0f) rate = 0.0f;
	if (rate > 1.0f) rate = 1.0f;
	parts->hscroll_rate = rate;
	// Если часть при этом ещё и полоса прокрутки — подвинуть ползунок как обычно.
	if (parts->is_hscrollbar)
		PE_SetPartsHScrollbarScrollRate(parts_no, rate);
}

float PE_GetHSliderBarScrollRate(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	float r = parts ? parts->hscroll_rate : 0.0f;
	// XSYS4_SLIDER_TRACE=1 — кто и когда СПРАШИВАЕТ долю. Отвечает на вопрос, который
	// одним дампом частей не закрыть: игра узнаёт о протяжке опросом или событием.
	if (getenv("XSYS4_SLIDER_TRACE"))
		NOTICE("SLIDER get no=%d rate=%.3f", parts_no, r);
	return r;
}

float PE_GetPartsHScrollbarScrollRate(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->hscroll_rate : 0.0f;
}

// Vertical scrollbar (パーツタイプ=2): mirror of the horizontal slider on the Y axis.
// The knob (the part's CG) slides down the track between the ∧/∨ arrow buttons, which
// reserve up_size/down_size at the top/bottom. rate 0 = top (newest scrolled away),
// 1 = bottom.
static void parts_vscrollbar_reposition(struct parts *parts)
{
	if (!parts->is_vscrollbar)
		return;
	struct parts_common *c = &parts->states[PARTS_STATE_DEFAULT].common;
	int knob_w = c->w, knob_h = c->h;
	float r = parts->vscroll_rate;
	if (r < 0.0f) r = 0.0f;
	if (r > 1.0f) r = 1.0f;
	int track = parts->sb_length - parts->sb_up_size - parts->sb_down_size;
	int travel = track - knob_h;
	if (travel < 0) travel = 0;
	int y = parts->sb_base_y + parts->sb_up_size + (int)(r * travel + 0.5f);
	int x = parts->sb_base_x + (parts->sb_width - knob_w) / 2;
	parts_set_pos(parts, (Point){ x, y });
}

// Шаг позиции за одно нажатие кнопки-стрелки скроллбара (.pactex `ボタンクリック移動量`).
// Им листаются сцены бэк-сцены и строки бэклога: `PressNextButton`/`PressPrevButton`
// прибавляют этот шаг к текущей позиции и ставят её обратно.
void PE_SetVScrollbarMoveSizeByButton(int parts_no, int size)
{
	parts_get(parts_no)->sb_move_by_button = size;
}

int PE_GetVScrollbarMoveSizeByButton(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	// Дефолт 1, а не 0: нулевой шаг = кнопки-стрелки не делают ничего.
	if (!parts || parts->sb_move_by_button == 0)
		return 1;
	return parts->sb_move_by_button;
}

void PE_InitPartsVScrollbar(int parts_no, int base_x, int base_y, int length, int width,
		int up_size, int down_size, int total, int view, float rate)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return;
	parts->is_vscrollbar = true;
	parts->sb_base_x = base_x;
	parts->sb_base_y = base_y;
	parts->sb_length = length;
	parts->sb_width = width;
	parts->sb_up_size = up_size;
	parts->sb_down_size = down_size;
	parts->sb_total = total;
	parts->sb_view = view;
	parts->vscroll_rate = rate;
	parts_vscrollbar_reposition(parts);
}

// Set the knob position from a rate (0..1); called when the game moves the scrollbar
// via SetVScrollbarScrollPos (pos/total/view converted to a rate in PartsEngine).
void PE_SetPartsVScrollbarRate(int parts_no, float rate)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || !parts->is_vscrollbar)
		return;
	parts->vscroll_rate = rate;
	parts_vscrollbar_reposition(parts);
}

// Drag the knob so its centre follows the cursor (absolute window y), clamped to the
// track between the arrow buttons; update vscroll_rate (the game reads it back / we
// feed it to SetVScrollbarScrollPos).
void parts_vscrollbar_drag_to(struct parts *parts, int cursor_abs_y)
{
	if (!parts->is_vscrollbar)
		return;
	struct parts_common *c = &parts->states[PARTS_STATE_DEFAULT].common;
	int knob_h = c->h;
	int track = parts->sb_length - parts->sb_up_size - parts->sb_down_size;
	int travel = track - knob_h;
	if (travel <= 0)
		return;
	int parent_y = parts->parent ? parts->parent->global.pos.y : 0;
	int knob_top = cursor_abs_y - parent_y - parts->sb_base_y - parts->sb_up_size - knob_h / 2;
	float r = (float)knob_top / (float)travel;
	if (r < 0.0f) r = 0.0f;
	if (r > 1.0f) r = 1.0f;
	parts->vscroll_rate = r;
	parts_vscrollbar_reposition(parts);
}

// Solid white fill of w×h into every display state. Used for config colour
// swatches: パーツタイプ=1 with an empty ＣＧ名 but a サイズ — the fill is tinted
// by the checkbox "button colour" (multiply) to show the palette colour.
void PE_SetPartsColorFill(int parts_no, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	struct parts *parts = parts_get(parts_no);
	for (int s = 0; s < PARTS_NR_STATES; s++) {
		struct parts_cg *cg = parts_get_cg(parts, s);
		gfx_delete_texture(&cg->common.texture);
		gfx_init_texture_rgba(&cg->common.texture, w, h, (SDL_Color){255, 255, 255, 255});
		parts_set_dims(parts, &cg->common, w, h);
	}
	parts_dirty(parts);
}

static void parts_checkbox_reload_cg(struct parts *parts);

// Checkbox state. The config screen toggles and reads these back.
void PE_SetPartsCheckBoxChecked(int parts_no, bool checked)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return;
	bool nc = !!checked;
	if (parts->is_checkbox && nc != parts->checkbox_checked) {
		parts->checkbox_checked = nc;
		parts_checkbox_reload_cg(parts);
	} else {
		parts->checkbox_checked = nc;
	}
}

bool PE_GetPartsCheckBoxChecked(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	bool r = parts ? parts->checkbox_checked : false;
	if (getenv("XSYS4_CB_TRACE"))
		NOTICE("CB get part %d -> %d%s", parts_no, r, parts ? "" : " (части нет)");
	return r;
}

// Load the checkbox box CG for the current checked state into all three display
// states (通常/オン/ダウン). Checked adds a "／チェック" segment before the state
// suffix, e.g. "コンフィグ／チェックボックス／チェック／通常".
static void parts_checkbox_reload_cg(struct parts *parts)
{
	if (!parts->is_checkbox || !parts->checkbox_cg_base)
		return;
	static const char *const sfx[4] = { NULL, "／通常", "／オン", "／ダウン" };
	// НЕДОСТУПНЫЙ чекбокс во ВСЕХ трёх состояниях показывает `／無効`: наведение и
	// нажатие на него ничего не меняют, значит и вида «оン»/«ダウン» у него быть не
	// должно. Файлы есть отдельно и для отмеченного (`／チェック／無効`), поэтому
	// галочка при гашении не пропадает — ровно как в оригинале, где четыре выбранных
	// пункта System Menu остаются с галочками, а остальные сереют.
	static const char *const sfx_off[4] = { NULL, "／無効", "／無効", "／無効" };
	const char *const *suffix = parts->checkbox_enabled ? sfx : sfx_off;
	const char *chk = parts->checkbox_checked ? "／チェック" : "";
	int base_no = parts->no;
	for (int s = 1; s <= 3; s++) {
		char u8[64];
		int n = 0;
		for (const char *p = chk; *p; p++) u8[n++] = *p;
		for (const char *p = suffix[s]; *p; p++) u8[n++] = *p;
		u8[n] = '\0';
		char *sjis = utf2sjis(u8, n);
		struct string *suf = make_string(sjis, strlen(sjis));
		struct string *full = string_concatenate(parts->checkbox_cg_base, suf);
		bool ok = PE_SetPartsCG(base_no, full, 0, s);
		if (getenv("XSYS4_CB_TRACE"))
			NOTICE("CB reload part %d state %d cg='%s' -> %d", base_no, s, full->text, ok);
		free_string(full);
		free_string(suf);
		free(sjis);
	}
}

void PE_InitPartsCheckBox(int parts_no, struct string *cg_base, bool checked)
{
	// parts_get (not parts_try_get): the part may not exist yet — this is the
	// first call for a checkbox part, and it must be created before SetPartsCG.
	struct parts *parts = parts_get(parts_no);
	if (!parts)
		return;
	parts->is_checkbox = true;
	if (parts->checkbox_cg_base)
		free_string(parts->checkbox_cg_base);
	parts->checkbox_cg_base = cg_base ? string_ref(cg_base) : NULL;
	parts->checkbox_checked = checked;
	parts_checkbox_reload_cg(parts);
}

/*
 * `ユーザコンポーネント` — см. комментарий у полей в parts_internal.h. Имя и
 * набор «ключ→значение» просто хранятся: их кладёт загрузчик раскладки из узла
 * 種類別情報, а читает игра (`parts::detail::CUserComponentParts@ComponentName::get`
 * @0x3bd0ac и `@GetTextData` @0x3bd1e4), чтобы найти зарегистрированный класс
 * компонента и создать его экземпляр поверх этой части.
 */
void PE_SetUserComponentName(int parts_no, struct string *name)
{
	struct parts *parts = parts_get(parts_no);
	parts->is_user_component = true;
	if (parts->user_component_name)
		free_string(parts->user_component_name);
	parts->user_component_name = name ? string_ref(name) : NULL;
}

struct string *PE_GetUserComponentName(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return string_ref(parts && parts->user_component_name
			? parts->user_component_name : &EMPTY_STRING);
}

void PE_SetUserComponentData(int parts_no, struct string *key, struct string *value)
{
	if (!key)
		return;
	struct parts *parts = parts_get(parts_no);
	for (int i = 0; i < parts->nr_user_component_data; i++) {
		if (strcmp(parts->user_component_data[i].key->text, key->text))
			continue;
		free_string(parts->user_component_data[i].value);
		parts->user_component_data[i].value =
			string_ref(value ? value : &EMPTY_STRING);
		return;
	}
	parts->user_component_data = xrealloc_array(parts->user_component_data,
		parts->nr_user_component_data, parts->nr_user_component_data + 1,
		sizeof(struct parts_uc_data));
	parts->user_component_data[parts->nr_user_component_data].key = string_ref(key);
	parts->user_component_data[parts->nr_user_component_data].value =
		string_ref(value ? value : &EMPTY_STRING);
	parts->nr_user_component_data++;
}

struct string *PE_GetUserComponentData(int parts_no, struct string *key)
{
	struct parts *parts = parts_try_get(parts_no);
	struct string *found = &EMPTY_STRING;
	if (parts && key) {
		for (int i = 0; i < parts->nr_user_component_data; i++) {
			if (!strcmp(parts->user_component_data[i].key->text, key->text)) {
				found = parts->user_component_data[i].value;
				break;
			}
		}
	}
	return string_ref(found);
}

// Toggle on user click; the game reads the new state via IsCheckBoxChecked.
// Возвращает true, если состояние действительно изменилось: по этому признаку
// обработчик ввода решает, слать ли игре сообщение CHANGED_FLG. Недоступный
// чекбокс кликом не переключается — иначе в System Menu можно было бы набрать
// больше четырёх ярлыков, чем игра и не рассчитывает управлять.
bool parts_checkbox_toggle(struct parts *parts)
{
	if (!parts->is_checkbox || !parts->checkbox_enabled)
		return false;

	/*
	 * РАДИОКНОПКА (кнопка внутри `ラジオボタンボックス`) ведёт себя иначе, чем чекбокс:
	 * клик по ней ВЫБИРАЕТ её и гасит остальные кнопки группы, а повторный клик по уже
	 * выбранной ничего не меняет. Исключительность — на ДВИЖКЕ: замером проверено, что
	 * игра её не наводит (после `SetScalingType` обе кнопки продолжали гореть, пока
	 * гашение не начал делать движок).
	 *
	 * Погашенным соседям событий не шлём: их обработчик-«если включили» получил бы ноль
	 * после нашей единицы и обнулил только что выбранное.
	 */
	struct parts *box = parts_radio_box_of(parts, NULL);
	if (box) {
		for (int i = 0; i < box->nr_radio_children; i++) {
			if (box->radio_children[i] == parts->no)
				continue;
			struct parts *other = parts_try_get(box->radio_children[i]);
			if (other && other->checkbox_checked) {
				other->checkbox_checked = false;
				parts_checkbox_reload_cg(other);
			}
		}
		if (parts->checkbox_checked)
			return false;          // уже выбрана — ничего не изменилось
		parts->checkbox_checked = true;
		parts_checkbox_reload_cg(parts);
		return true;
	}

	parts->checkbox_checked = !parts->checkbox_checked;
	parts_checkbox_reload_cg(parts);
	return true;
}

void PE_SetPartsCheckBoxEnable(int parts_no, bool enable)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return;
	bool en = !!enable;
	if (parts->checkbox_enabled == en)
		return;
	parts->checkbox_enabled = en;
	if (getenv("XSYS4_CB_TRACE"))
		NOTICE("CB enable part %d <- %d (checked=%d)", parts_no, (int)en,
		       (int)parts->checkbox_checked);
	parts_checkbox_reload_cg(parts);
}

bool PE_GetPartsCheckBoxEnable(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->checkbox_enabled : true;
}

// Checkbox label colour. Stored so the game can read it back; the checkbox is
// not yet drawn as a distinct widget.
void PE_SetPartsCheckBoxColor(int parts_no, int r, int g, int b)
{
	if (getenv("XSYS4_MUL_TRACE"))
		NOTICE("CBCOLOR part %d <- rgb %d,%d,%d", parts_no, r, g, b);
	struct parts *parts = parts_try_get(parts_no);
	if (parts) {
		parts->checkbox_r = r;
		parts->checkbox_g = g;
		parts->checkbox_b = b;
		// The checkbox "button colour" tints the box CG — this is how the config
		// message-colour swatches show their palette (a single grey CG multiplied
		// by each colour). Normal checkboxes use white here, so this is a no-op.
		parts_set_multiply_color(parts, (SDL_Color){
			min(255, max(0, r)), min(255, max(0, g)), min(255, max(0, b)), 255 });
	}
}

int PE_GetPartsCheckBoxR(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->checkbox_r : 0;
}

int PE_GetPartsCheckBoxG(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->checkbox_g : 0;
}

int PE_GetPartsCheckBoxB(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->checkbox_b : 0;
}

/*
 * Создать часть с дефолтами движка, если её ещё нет.
 *
 * Загрузчик раскладок нумерует части сам, а РЕАЛЬНО часть появляется как
 * побочный эффект первого сеттера, который пользуется `parts_get`
 * (PE_SetPartsCG/PE_SetPartsFlat/...). Состоянию «прямоугольная часть»
 * (`矩形パーツ`) грузить нечего — это чистая область попадания, — а
 * `PE_SetPartsRectangleDetectionSize` намеренно НЕ создаёт часть
 * (`parts_try_get`, HLL-семантика: на несуществующий номер вернуть false),
 * поэтому загрузчику нужен явный способ её создать.
 */
// Пометить часть как маску построения (см. construction_mask в parts_internal.h):
// содержимое `構築パーツ` не построено, поэтому заливка-заглушка годится только
// как прямоугольная маска альфа-клиппера, но не как то, что видно на экране.
void PE_SetPartsConstructionMask(int parts_no)
{
	struct parts *parts = parts_get(parts_no);
	parts->construction_mask = true;
	parts_dirty(parts);
}

void PE_EnsureParts(int parts_no)
{
	parts_get(parts_no);
}

bool PE_SetPartsRectangleDetectionSize(int parts_no, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return false;
	if (parts->states[state].type != PARTS_RECT_DETECTION)
		parts_state_reset(&parts->states[state], PARTS_RECT_DETECTION);
	parts_set_dims(parts, &parts->states[state].common, w, h);
	return true;
}

/*
 * `ＣＧ判定パーツ`: hit-область по форме картинки. Картинку грузим как обычно,
 * но состояние помечаем PARTS_CG_DETECTION — рендер такое состояние пропускает,
 * а hit-тест идёт по непрозрачным пикселям текстуры (тот же путь, что у
 * `マウスカーソルピクセル判定`). Игра ищет такие части сравнением типа компонента
 * (`CActivityWrap@GetCGDetection` → CompParts(имя, 27, 1) у v14), поэтому одним
 * лишь CG-состоянием обойтись нельзя: без своего типа `FooterButton.jaf:53`
 * ронял игру ассертом «(nonnull) m_act.GetCGDetection("Detector")».
 */
bool PE_SetPartsCGDetectionSize(int parts_no, struct string *cg_name, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts *parts = parts_get(parts_no);
	if (!cg_name || *(cg_name->text) == '\0') {
		parts_state_reset(&parts->states[state], PARTS_CG_DETECTION);
		parts_dirty(parts);
		return true;
	}
	// Грузим CG обычным путём (он выставит размеры состояния), затем меняем тип
	// состояния на «только определение попадания»: parts_state_reset тут звать
	// нельзя — он же и снёс бы только что загруженную текстуру.
	struct parts_cg *cg = parts_get_cg(parts, state);
	bool ok = parts_cg_set(parts, cg, cg_name);
	parts->states[state].type = PARTS_CG_DETECTION;
	parts_dirty(parts);
	return ok;
}

/*
 * NOTE: per GUIEngine.dll (Rance 01) — МОНОТОННЫЙ счётчик, номера не
 * переиспользуются. Эта же семантика верна и для PartsEngine новых игр.
 *
 * ★ОТВЕРГНУТО ЗАМЕРОМ, не возвращать: «чистый скан от базы БЕЗ памяти между
 * вызовами» (бывший PE_GetFreeNumberScan из c987c19). Пока парт не
 * материализован, скан выдаёт ОДИН номер ВСЕМ подряд идущим вызовам — а игровые
 * Create-пути между взятиями номера движку не сообщают НИЧЕГО (вся
 * «занятость» живёт в игровых структурах: `AFL_Parts_CreateSprite(0)` →
 * `GetFreeSystemPartsNumber` → `NEW CSpriteParts` → CParts@0 → Number::set +
 * Deleted-подписка в CPartsMessageManager — ни одного HLL-вызова). У Dohna
 * `AdvEventCg@0` зовёт `NsfwCGParts::Create` ДВАЖДЫ (фон и оверлей) — оба
 * получали 1000001022, и оверлей штатным `SetPartsCG(n,"")` стирал только что
 * установленный фон пролога (замер FREENUM-стеками: оба владельца — соседние
 * адреса 0x66833a/0x66839e в AdvEventCg@0; кадр: текст ADV на чёрном).
 * Кейс, ради которого скан вводился (виджет юнит-анимации Haha Ranman,
 * «контейнер+фон+рамка должны слиться в один парт»), оказался ЛОЖНОЙ моделью:
 * `CUnitAnimation@0#2` у HR тоже зовёт `AFL_Parts_CreateSprite` дважды — как у
 * Dohna, то есть у оригинала это РАЗНЫЕ парты; серый прямоугольник чинила
 * вторая половина c987c19 (delegate-индексы у RemoveController/ReleaseActivity)
 * вместе с заявками SetComponentType. Проверено на счётчике: пролог HR чист
 * (плёнка 0, юнит-аниме 0, кнопок 11), фон пролога Dohna на месте.
 */
int PE_GetFreeNumber(void)
{
	static int first_free = 1000001000;
	while (PE_IsExist(first_free)) {
		first_free++;
	}
	// XXX: the ID is incremented even if the parts is not created
	if (getenv("XSYS4_PARTS_TRACE"))
		NOTICE("PARTS GetFreeNumber -> %d", first_free);
	// XSYS4_FREENUM_STACK=<номер>: стек игры на выдаче этого номера —
	// отвечает «кто взял номер» (им и найден дубль-владелец при скане).
	{
		const char *w = getenv("XSYS4_FREENUM_STACK");
		if (w && atoi(w) == first_free) {
			NOTICE("FREENUM %d выдан:", first_free);
			vm_stack_trace();
		}
	}
	return first_free++;
}

bool PE_IsExist(int parts_no)
{
	return !!ht_get_int(parts_table, parts_no, NULL);
}

void PE_SetSpeedupRateByMessageSkip(int parts_no, int rate)
{
	if (rate != 1)
		UNIMPLEMENTED("(%d, %d)");
}

// Порядок слоёв изменился (добавили/сняли слой) — ключ сортировки у ВСЕХ партов
// зависит от позиции, поэтому список надо пересобрать целиком.
static void ctrl_stack_resort_all(void);

/*
 * ПОРЯДОК СПИСКА УСТАРЕВАЕТ — проверяем раз в кадр перед вводом. Список частей
 * отсортирован по (позиция слоя, global.z), и обработка ввода идёт по нему
 * front-to-back; кто первый дал hit, тот и забирает курсор. Если чья-то `global.z`
 * поменялась без `parts_list_resort`, порядок расходится с z — и курсор достаётся
 * НЕ ТОЙ части.
 *
 * Живой случай (отчёт пользователя): при ПОВТОРНОМ открытии экрана Load слоты
 * переставали нажиматься. Дамп по сигналу: курсор 330,242 — над слотом 90000531
 * (`z=19`, область 28..656 × 191..285), а курсор забрала полноэкранная панель
 * 1000001306 (`z=1`) ИЗ ТОГО ЖЕ слоя 12, потому что в списке стояла ПОСЛЕ слота.
 * Клики при этом доходили (3643 кадра с зажатой кнопкой), но `clicked_parts`
 * оставался нулевым.
 *
 * Проверка стоит O(n) на кадр (у Dohna это 100–200 частей) и срабатывает молча;
 * при расхождении список пересобирается целиком тем же путём, что и при смене
 * стека слоёв.
 */
bool parts_list_order_check(void)
{
	// Откат для A/B на одном бинаре: XSYS4_NO_ORDER_CHECK=1 — прежнее поведение,
	// когда устаревший порядок оставался как есть.
	static const char *off = (const char *)1;
	if (off == (const char *)1)
		off = getenv("XSYS4_NO_ORDER_CHECK");
	if (off && *off)
		return false;

	struct parts *p, *prev = NULL;
	bool bad = false;
	PARTS_LIST_FOREACH(p) {
		if (prev) {
			int az = parts_get_sprite_z(prev), az2 = parts_get_sprite_z2(prev);
			int bz = parts_get_sprite_z(p), bz2 = parts_get_sprite_z2(p);
			if (az > bz || (az == bz && az2 > bz2)) {
				bad = true;
				break;
			}
		}
		prev = p;
	}
	if (!bad)
		return false;
	if (getenv("XSYS4_ORDER_TRACE"))
		NOTICE("PARTS: порядок списка разошёлся с z — пересортировка");
	ctrl_stack_resort_all();
	return true;
}

static void ctrl_stack_resort_all(void)
{
	struct parts *p;
	struct parts_list saved;
	TAILQ_INIT(&saved);
	while ((p = TAILQ_FIRST(&parts_list))) {
		parts_list_remove(p);   // снимает и с TAILQ, и со сцены спрайтов
		TAILQ_INSERT_TAIL(&saved, p, parts_list_entry);
	}
	while ((p = TAILQ_FIRST(&saved))) {
		TAILQ_REMOVE(&saved, p, parts_list_entry);
		parts_list_insert(p);
	}
}

// Позиция слоя `id` в стеке (низ = 0) или -1, если такого слоя нет.
static int ctrl_stack_pos(int id)
{
	for (int i = 0; i < ctrl_stack.nr_controllers; i++)
		if (ctrl_stack.stack[i] == id)
			return i;
	return -1;
}

// Восстановление стека после загрузки сейва: в сейве лежит только ГЛУБИНА (формат
// менять нельзя), а нумерация там всегда была стековой — значит id = позиция + 1
// (ID слоёв 1-based, см. ctrl_stack_init). `active` приходит уже сдвинутым:
// пересчитать 0-based значение старого образа может только загрузчик, который
// знает его версию.
void parts_controller_stack_restore(int nr, int active)
{
	if (nr < 0) nr = 0;
	if (nr > PARTS_CONTROLLER_STACK_MAX) nr = PARTS_CONTROLLER_STACK_MAX;
	ctrl_stack.nr_controllers = nr;
	for (int i = 0; i < nr; i++) {
		ctrl_stack.stack[i] = i + 1;
		ctrl_stack.hidden[i] = false;
	}
	ctrl_stack.next_id = nr + 1;
	ctrl_stack.active = active;
}

static void ctrl_stack_init(void)
{
	memset(&ctrl_stack, 0, sizeof(ctrl_stack));
	/*
	 * ID СЛОЁВ НАЧИНАЮТСЯ С 1. Игра оборачивает ID слоя как КОМПОНЕНТ
	 * (`CBackSceneView@ShowAllFrontScene` → `parts::detail::Wrap`), а валидность
	 * компонента у неё — `<Number> != 0`: обёртка слоя с ID 0 рождается мёртвой,
	 * игра её сохраняет и ассертит `parts.IsValid` на первой же операции
	 * (маршрут scrollback → Back, 38 нулевых обёрток за прогон; FINDINGS §5df).
	 */
	ctrl_stack.next_id = 1;
	// Add initial default controller
	PE_AddController(-1);
}

// Adds a new controller to the stack and makes it active. The `index`
// parameter specifies the position in the stack at which to insert the new
// controller; -1 means "insert directly after the currently active
// controller". In practice the game only ever passes -1, and the active
// controller is always the top of the stack at that point, so this
// degenerates to a simple push.
int PE_AddController(int index)
{
	if (ctrl_stack.nr_controllers >= PARTS_CONTROLLER_STACK_MAX)
		VM_ERROR("controller stack overflow");

	// ID монотонный (см. struct parts_controller_stack): номера НЕ переиспользуются.
	int no = ctrl_stack.next_id++;
	int pos = ctrl_stack.nr_controllers++;
	ctrl_stack.stack[pos] = no;
	ctrl_stack.hidden[pos] = false;
	ctrl_stack.active = no;
	ctrl_stack_resort_all();   // позиции слоёв — ключ порядка, см. parts_get_sprite_z
	// XSYS4_CTRL_TRACE_AR=1 — только Add/Remove слоёв, без пер-партового спама
	// (полный XSYS4_CTRL_TRACE упирается в лимит лога задолго до конца прогона).
	if (getenv("XSYS4_CTRL_TRACE") || getenv("XSYS4_CTRL_TRACE_AR")) {
		NOTICE("PE_AddController(index=%d) -> ctrl %d (nr=%d) [%s]", index, no,
		       ctrl_stack.nr_controllers, display_sjis0(vm_current_function_name()));
		if (getenv("XSYS4_CTRL_TRACE_AR"))
			vm_stack_trace();
	}
	return no;
}

// Removes the controller at position `index` from the stack, releases all
// parts belonging to it, and returns their parts numbers in
// `erase_number_list`. `index == -1` means "remove the currently active
// controller". In practice the game only ever passes -1, and the active
// controller is always the top of the stack at that point, so this
// degenerates to a simple pop.
void PE_RemoveController(struct page **erase_number_list, int index)
{
	// index != -1 у новых игр — удаляем текущий активный (верх стека).
	if (ctrl_stack.nr_controllers == 0)
		return;

	/*
	 * ★АРГУМЕНТ ЧИТАЕТСЯ КАК ID СЛОЯ (2026-08-08). Прежде он игнорировался и
	 * сносился активный слой — шесть замеров подряд давали с трактовкой «ID»
	 * чёрный экран, потому что СЦЕНЫ БЫЛИ БЕССМЕРТНЫ и их парты всё равно
	 * оставались. После того как сцены стали умирать, замер повторён и трактовка
	 * подтвердилась:
	 *   • пролог играется как прежде (кадр совпадает с контрольным прогоном
	 *     с точностью до фазы дождя, 0 `*ERROR*`);
	 *   • на загрузке сейва она РЕШАЕТ дефект: игра просит снести слой 17, а
	 *     активным к этому моменту стоит 20 (слой LOAD-экрана текущего сеанса) —
	 *     со «сношу активный» на экране оставался мёртвый SAVE-оверлей вместо
	 *     ADV-сцены, а с чтением ID интерфейс ADV возвращается.
	 * Откат для замеров: `XSYS4_RC_ACTIVE=1` — снова сносить активный слой.
	 *
	 * По .ain аргумент — ПОЗИЦИЯ В СТЕКЕ, и API различает позицию и ID явно:
	 *   void RemoveController(wrap<array<int>> EraseNumberList, int Index);
	 *   int  AddController(int Index);      // возвращает ID
	 *   int  GetControllerIndex(int ID);    // ID  -> позиция
	 *   int  GetControllerID(int Index);    // позиция -> ID
	 * Трактовку «ID» подтверждает и игровой код: подписки на покадровое событие
	 * помечаются слоем из `GetActiveController`, а `EraseLayer(id)` тем же id их и
	 * снимает (`ResetPartsUpdateEventLayerID`) — то есть игра адресует слои своими
	 * номерами, а не позициями. Трактовка «позиция в стеке» проверена и отвергнута
	 * (тот же чёрный экран, что у прежних замеров «ID» до починки смерти сцен).
	 *
	 * ★ОСТАЁТСЯ ОТКРЫТЫМ: наша ПРИВЯЗКА ПАРТОВ К СЛОЯМ (`parts_init`:
	 * `controller_no = ctrl_stack.active`) местами расходится с игровой. Видно
	 * ровно на загрузке: игра сносит слой 17 и считает дело сделанным, а парты
	 * SAVE-экрана лежат у нас на 18 и остаются на экране. Мерить так:
	 * `XSYS4_CTRL_TRACE_AR=1` печатает запрошенный index, снесённый слой, сколько
	 * партов освобождено и раскладку партов по слоям при пустом сносе.
	 */
	bool rc_id = !getenv("XSYS4_RC_ACTIVE");
	bool was_active = !rc_id || index < 0 || index == ctrl_stack.active;
	int ctrl_no;
	int pos;
	if (rc_id) {
		ctrl_no = index >= 0 ? index : ctrl_stack.active;
		pos = ctrl_stack_pos(ctrl_no);
		if (pos < 0)
			ctrl_no = -1;
	} else {
		pos = ctrl_stack_pos(ctrl_stack.active);
		ctrl_no = (pos >= 0 && pos < ctrl_stack.nr_controllers)
				? ctrl_stack.stack[pos] : -1;
	}
	if (getenv("XSYS4_CTRL_TRACE") || getenv("XSYS4_CTRL_TRACE_AR")) {
		NOTICE("PE_RemoveController: index=%d -> сношу ctrl %d (позиция %d, nr=%d) [%s]",
		       index, ctrl_no, pos, ctrl_stack.nr_controllers,
		       display_sjis0(vm_current_function_name()));
		if (getenv("XSYS4_CTRL_TRACE_AR"))
			vm_stack_trace();
	}
	if (ctrl_no < 0) {
		// Вместо тихого «снесу что-нибудь» — проверка допущения.
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("RemoveController: позиции %d нет в стеке (глубина %d)",
				pos, ctrl_stack.nr_controllers);
		}
		return;
	}

	/*
	 * ★СЛОЙ СНОСИТ ВСЕ СВОИ ЧАСТИ, БЕЗ ИСКЛЮЧЕНИЙ — иначе часть переживает свой
	 * слой, а порядок вывода для такой части определить нечем (см.
	 * `parts_get_sprite_z`: ключ «слоя нет» совпадает с ключом САМОГО НИЖНЕГО
	 * слоя, то есть часть всплывает над ним, а не уходит под всё).
	 *
	 * ИСТОРИЯ (важна, чтобы не вернуть исключение обратно). Здесь стояла ветка
	 * `keep_act`: часть, числящаяся за живой активностью, со слоем не сносилась —
	 * якобы её время жизни игра ведёт сама, своим пулом экземпляров
	 * (`IdArray<string, ActivityInstances>`). Поводом был экран Load, открытый ИЗ
	 * ИГРЫ второй раз за сессию: `ActivityInstances@Request` доставал из пула
	 * ПРЕЖНИЙ экземпляр (`Array.First`, без `IsLoaded`/`ReadFile`) и просил
	 * `GetActivityPartsNumber(…, "Button")` — тот отдавал номер уже снесённой
	 * части, и игра ассертила `(nonnull) m_act.GetButton("Button")`
	 * (`SaveThumbnailView.jaf:21`).
	 *
	 * Настоящая причина была не в сносе частей, а в §5es: `vm_call_hll_func`
	 * брал ссылку на одолженный аргумент лямбды, и `Array.First`/`Any`/`Find`
	 * НАКАЧИВАЛИ счётчик экземпляра из пула — тот не умирал и при следующем
	 * открытии выдавался как живой. После правки владения экземпляр умирает, а
	 * `Request` честно перезагружает раскладку: замер на том же сценарии (Load из
	 * игры дважды подряд) даёт `ReadActivityFile` и НОВЫЕ экземпляры
	 * `SaveThumbnailView:2/3/4` вместо переиспользования `:0/:1`, ассерта нет,
	 * `*ERROR*` 0. Откат для замеров: `XSYS4_KEEP_ACT_PARTS=1`.
	 */
	/*
	 * ★ПОПРАВКА К §5et (замер пользователя, §5ev): исключение ВЕРНУЛОСЬ, но уже
	 * не «на всякий случай», а по факту — часть ЖИВОЙ активности слой не сносит.
	 *
	 * Улика — реплики ADV без текста со второго эпизода. Порядок такой:
	 * `AdvMessageWindow_main` читается ОДИН раз (`CreateActivity` один,
	 * `ReleaseActivity` нет), окно строится на слое 5; конец эпизода —
	 * `SceneContext@1` → `EraseLayer` сносит слой, и вместе с ним умирало окно
	 * (`parts_release` освобождает `parts->mw`). В следующем эпизоде игра
	 * активность НЕ перечитывает — она для неё жива — и обращается к окну по
	 * прежнему номеру (`AdvMessageWindow@Show` → `CMessageWindow@IsShow` →
	 * `CParts@Show::get`). Мы на это заводили ПУСТУЮ часть, и
	 * `SetMessageWindowText` писать было некуда: текста нет вовсе.
	 *
	 * Почему это не возвращает старую беду с порядком вывода: часть без слоя
	 * теперь НЕ РИСУЕТСЯ (`parts_hidden_by_layer`), то есть ждёт, пока игра к ней
	 * обратится и `parts_adopt_to_active_layer` вернёт её на актуальный слой.
	 * Раньше она оставалась видимой с ключом нижнего слоя и всплывала над фоном —
	 * это и были остатки боя на экране.
	 *
	 * `XSYS4_KEEP_ACT_PARTS=0` — снести ВСЁ, включая части живых активностей
	 * (прежнее поведение §5et, для A/B).
	 */
	bool pe_parts_in_activity(int parts_no);
	const char *keep_env = getenv("XSYS4_KEEP_ACT_PARTS");
	bool keep_act = !keep_env || strcmp(keep_env, "0");
	int released = 0;
	const char *sample = "";
	struct parts *p = TAILQ_FIRST(&parts_list);
	while (p) {
		struct parts *next = TAILQ_NEXT(p, parts_list_entry);
		if (p->controller_no == ctrl_no && keep_act && pe_parts_in_activity(p->no)) {
			// Под XSYS4_CTRL_TRACE_AR видно и УЦЕЛЕВШИХ: часть активности со слоем
			// не сносится, но её delegate-индекс и не отдаётся игре.
			if (getenv("XSYS4_CTRL_TRACE_AR"))
				NOTICE("   ctrl %d: ОСТАВЛЕН активити-парт %d (delegate=%d)",
				       ctrl_no, p->no, p->delegate_index);
			p = next;
			continue;
		}
		if (p->controller_no == ctrl_no) {
			if (getenv("XSYS4_CTRL_TRACE") || getenv("XSYS4_CTRL_TRACE_AR")) {
				released++;
				NOTICE("   ctrl %d: снят парт %d (delegate=%d, activity=%d, cg=\"%s\")",
				       ctrl_no, p->no, p->delegate_index,
				       pe_parts_in_activity(p->no),
				       p->states[0].type == PARTS_CG && p->states[0].cg.name
				               ? display_sjis0(p->states[0].cg.name->text) : "");
				if (!*sample && p->states[0].type == PARTS_CG
						&& p->states[0].cg.name)
					sample = display_sjis0(p->states[0].cg.name->text);
			}
			/*
			 * ★В массив идут DELEGATE-ИНДЕКСЫ, а не номера партов. Так его читает
			 * сама игра: `parts::detail::EraseLayer` называет локальную
			 * переменную `delegateIndexList` и сразу передаёт её в
			 * `CPartsMessageManager@ReleaseFunctionSetList`, который в цикле
			 * зовёт `ReleaseFunctionSet(delegateIndex)`. Имя аргумента в
			 * HLL-декларации (`EraseNumberList`) сбивает с толку.
			 *
			 * Пока сюда клали `p->no`, игра снимала наборы обработчиков по
			 * мусорным индексам, а настоящие оставались жить — вместе с
			 * объектами, которые их держат. Отсюда «покадровое событие мёртвого
			 * экрана»: `title::CScreen@UpdateEvent` тикал ещё долго после сноса
			 * титула и каждый кадр воскрешал свой парт через SetPartsCG (белый
			 * прямоугольник плёночного шума 320x180 в углу пролога).
			 */
			if (p->delegate_index >= 0)
				*erase_number_list = array_pushback(*erase_number_list,
						(union vm_value){.i = p->delegate_index},
						AIN_ARRAY_INT, -1);
			parts_release(p->no);
		}
		p = next;
	}

	if (getenv("XSYS4_CTRL_TRACE") || getenv("XSYS4_CTRL_TRACE_AR")) {
		NOTICE("   ctrl %d: освобождено партов %d, delegate-индексов отдано %d, напр. cg=\"%s\"",
		       ctrl_no, released,
		       *erase_number_list ? (*erase_number_list)->nr_vars : 0, sample);
		// Снос пустого слоя — признак того, что парты легли не туда: печатаем, где
		// они на самом деле, и каков стек. Ровно этот вывод показал, что расходится
		// не чтение аргумента RemoveController, а привязка партов к слоям.
		if (released == 0) {
			int cnt[PARTS_CONTROLLER_STACK_MAX + 2] = {0};
			struct parts *q;
			PARTS_LIST_FOREACH(q) {
				int c = q->controller_no;
				if (c >= 0 && c <= PARTS_CONTROLLER_STACK_MAX)
					cnt[c]++;
				else
					cnt[PARTS_CONTROLLER_STACK_MAX + 1]++;
			}
			char buf[256]; int n = 0;
			for (int i = 0; i <= PARTS_CONTROLLER_STACK_MAX + 1 && n < 200; i++)
				if (cnt[i])
					n += snprintf(buf + n, sizeof(buf) - n, "%d:%d ", i, cnt[i]);
			buf[n] = 0;
			char st[128]; int m = 0;
			for (int i = 0; i < ctrl_stack.nr_controllers && m < 100; i++)
				m += snprintf(st + m, sizeof(st) - m, "%d ", ctrl_stack.stack[i]);
			st[m] = 0;
			NOTICE("   ПУСТОЙ СНОС: парты по слоям [%s], стек [%s], активный %d",
			       buf, st, ctrl_stack.active);
		}
	}
	// Вынимаем из стека, НЕ перенумеровывая остальные (id устойчив). Признак
	// «погашен» живёт по позиции, поэтому сдвигается вместе со стеком.
	for (int i = pos; i + 1 < ctrl_stack.nr_controllers; i++) {
		ctrl_stack.stack[i] = ctrl_stack.stack[i + 1];
		ctrl_stack.hidden[i] = ctrl_stack.hidden[i + 1];
	}
	ctrl_stack.nr_controllers--;
	ctrl_stack_resort_all();
	if (ctrl_stack.nr_controllers == 0) {
		PE_AddController(-1);
	} else if (was_active) {
		// Активный слой умер — активным становится верх стека. Если снесли
		// НЕ-активный, active НЕ трогаем: игра ничего не переключала, и
		// принудительный перескок отправлял бы новые парты на чужой слой.
		ctrl_stack.active = ctrl_stack.stack[ctrl_stack.nr_controllers - 1];
	}
}

void PE_set_active_controller(int controller_no)
{
	// ★ОТДЕЛЬНАЯ ручка, а не XSYS4_CTRL_TRACE: игра дёргает это КАЖДЫЙ КАДР (у Haha
	// Ranman — 3↔0 по три раза за кадр), и под общим трейсом 200 000 строк выносили
	// из лога всё остальное, включая сами Add/RemoveController.
	// Печатаем только РЕАЛЬНЫЕ смены: игра дёргает это каждый кадр одним и тем же
	// значением, и «печатать всё» выносит из лога остальное.
	if (getenv("XSYS4_CTRL_TRACE_ACTIVE") && controller_no != ctrl_stack.active)
		NOTICE("ACTIVE %d -> %d [%s]", ctrl_stack.active, controller_no,
		       display_sjis0(vm_current_function_name()));
	/*
	 * ★НЕСУЩЕСТВУЮЩИЙ ID — НЕ ОШИБКА. Игры зовут это со слоями, которых нет,
	 * ШТАТНО: `parts::detail::CallPartsUpdateEvent` (fno 9181) выставляет слой
	 * КАЖДОЙ подписки перед её обработчиком, даже если `IsExistLayer` = 0, а у
	 * подписок без слоя там ноль — Haha Ranman дёргает 3↔0 по три раза за кадр.
	 * Пока ID слоёв начинались с 0, ноль совпадал с базовым слоем и вызов
	 * проходил; с 1-based нумерацией (§5df) прежний `VM_ERROR` ронял HR на
	 * первом же кадре ADV («Invalid controller number: 0» → REPL). Оригинал
	 * с той же нумерацией обязан это терпеть — храним ID как есть: снятие с
	 * несуществующего слоя парты и так уводит под всё (parts_get_sprite_z = 0),
	 * а следующий SetActiveLayer игры возвращает живой слой.
	 */
	ctrl_stack.active = controller_no;
}

int PE_get_active_controller(void)
{
	/*
	 * `XSYS4_LAYER_SUB_TRACE=1` — чем помечается ПОДПИСКА на покадровое событие.
	 * Игра держит подписки в своём массиве `g_dgPartsBeginUpdateEvent`
	 * (`SPartsUpdateData { int Layer; DG_PARTS_UpdateHandler Event; }`),
	 * пишет в `Layer` результат ЭТОЙ функции (`parts::detail::AddPartsUpdateEvent`
	 * → `GetActiveLayer`) и потом снимает подписки по слою
	 * (`ResetPartsUpdateEventLayerID` из `EraseLayer(id)`). Если помеченный слой не
	 * совпадёт с id стираемого, подписка не снимется никогда — именно так у нас
	 * тикает `title::CScreen@UpdateEvent` после сноса титула и каждый кадр
	 * воскрешает свой парт (плёночный шум в углу пролога).
	 *
	 * Саму функцию игра дёргает КАЖДЫЙ КАДР, поэтому печатаем только вызовы
	 * из подписки — иначе лог заваливает всё остальное.
	 */
	if (getenv("XSYS4_LAYER_SUB_TRACE") && vm_called_from("AddPartsUpdateEvent")) {
		NOTICE("LAYERSUB подписка помечается слоем %d — стек вызовов игры:",
		       ctrl_stack.active);
		vm_stack_trace();
	}
	return ctrl_stack.active;
}

int PE_get_controller_length(void)
{
	if (getenv("XSYS4_CTRL_TRACE"))
		NOTICE("GetControllerLength -> %d", ctrl_stack.nr_controllers);
	return ctrl_stack.nr_controllers;
}

int PE_get_controller_id(int index)
{
	if (index < 0 || index >= ctrl_stack.nr_controllers)
		return -1;
	if (getenv("XSYS4_CTRL_TRACE"))
		NOTICE("GetControllerID(%d) -> %d", index, ctrl_stack.stack[index]);
	return ctrl_stack.stack[index];
}

// ID слоя -> его позиция в стеке. В движке этой функции не было вовсе, хотя .ain её
// объявляет (`int GetControllerIndex(int ID)`) — просто игры, которые уже работали,
// её не звали.
int PE_get_controller_index(int id)
{
	return ctrl_stack_pos(id);
}

int PE_get_system_controller(void)
{
	return PARTS_CONTROLLER_SYSTEM_OVERLAY;
}

// «Компонент» у AliceSoft — это и парт, и СЛОЙ (контроллер): `Ｐ＿表示設定` (=
// PartsEngine.SetComponentShow) игра зовёт как с номерами партов (у Tsumamigui 3 они
// всегда большие: 90000000+, 100000000+, 1000001xxx), так и с ID слоя из
// GetControllerID — а это индекс стека, т.е. МАЛОЕ число. Различаем по диапазону:
// валидный индекс контроллера, для которого парта с таким номером не существует.
// Без этого `HideAllFrontScene` прятал парт №0 (которого нет — parts_get его тут же
// и создавал пустышкой), экран игры оставался под сценой, и текст бэк-сцены ложился
// поверх живого (FINDINGS §5q).
bool parts_controller_is_layer(int id)
{
	if (ctrl_stack_pos(id) < 0)
		return false;
	return parts_try_get(id) == NULL;
}

void parts_controller_set_show(int id, bool show)
{
	int pos = ctrl_stack_pos(id);
	if (pos < 0)
		return;   // слоя уже нет — гасить нечего
	ctrl_stack.hidden[pos] = !show;
	if (getenv("XSYS4_CTRL_TRACE"))
		NOTICE("CTRL слой %d -> show=%d", id, show);
	struct parts *parts;
	PARTS_LIST_FOREACH(parts) {
		if (parts->controller_no == id)
			parts_dirty(parts);
	}
}

bool parts_controller_get_show(int id)
{
	int pos = ctrl_stack_pos(id);
	return pos < 0 ? false : !ctrl_stack.hidden[pos];
}

/*
 * Слой части по её номеру, или −1, если части уже нет. Нужна диагностике
 * реестра активностей (`pe_dump_activities`): по одному номеру не отличить
 * «часть снята» от «часть жива, но на чужом слое».
 */
int PE_parts_controller_no(int parts_no)
{
	struct parts *p = parts_try_get(parts_no);
	return p ? p->controller_no : -1;
}

bool parts_hidden_by_layer(struct parts *parts)
{
	if (parts->controller_no == PARTS_CONTROLLER_SYSTEM_OVERLAY)
		return false;
	int pos = ctrl_stack_pos(parts->controller_no);
	/*
	 * ★ЧАСТЬ ЖИВОЙ АКТИВНОСТИ, ПЕРЕЖИВШАЯ СВОЙ СЛОЙ, НЕ РИСУЕТСЯ (§5et/§5ev).
	 *
	 * Такая часть жива намеренно: её время жизни ведёт игра своим пулом
	 * активностей, и `EraseLayer` для неё — не смерть, а «сцена закрылась».
	 * Рисовать её нельзя: позиции в стеке слоёв у неё нет, а ключ сортировки
	 * «нижний слой» (0) поднимал бы её НАД фоном — именно так на экране
	 * оставались остатки боя. Когда игра обратится к ней снова,
	 * `parts_adopt_to_active_layer` вернёт её на текущий слой, и она снова
	 * появится. То есть невидимость — состояние ожидания, а не потеря.
	 */
	if (pos >= 0)
		return ctrl_stack.hidden[pos];
	/*
	 * ★У части БЕЗ АКТИВНОСТИ слоя быть не должно вовсе: за ней никто не следит,
	 * усыновить её некому (`parts_adopt_to_active_layer` зовётся по обращению
	 * игры к окну реплик), и невидимость для неё — не ожидание, а тихая пропажа
	 * с экрана. Раньше такой случай был виден по предупреждению в
	 * `parts_get_sprite_z`; оно ушло вместе с той веткой, поэтому говорим здесь —
	 * иначе следующий «часть есть, а на экране пусто» придётся искать заново.
	 */
	// ★Проверка «чья часть» — ДО первого предупреждения и только до него:
	// pe_parts_in_activity обходит весь реестр активностей, а сюда приходят
	// рендер и ввод по каждой части каждый кадр. После первого раза ветка не
	// нужна вовсе, поэтому флаг гасит не печать, а сам обход.
	static bool warned = false;
	if (unlikely(!warned)) {
		bool pe_parts_in_activity(int parts_no);
		if (!pe_parts_in_activity(parts->no)) {
			warned = true;
			WARNING("часть %d числится за слоем %d, которого нет в стеке, и НЕ "
				"принадлежит активности: рисовать её негде и усыновить некому",
				parts->no, parts->controller_no);
		}
	}
	return true;
}

void PE_parts_set_want_save(int parts_no, bool want_save)
{
	parts_get(parts_no)->want_save = want_save;
}

void PE_parts_set_want_save_back_scene(int parts_no, bool want)
{
	parts_get(parts_no)->want_save_back_scene = want;
}

bool PE_parts_get_want_save_back_scene(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->want_save_back_scene : false;
}

float PE_parts_get_absolute_x(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? (float)parts->global.pos.x : 0.0f;
}

float PE_parts_get_absolute_y(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? (float)parts->global.pos.y : 0.0f;
}

int PE_parts_get_absolute_z(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->global.z : 0;
}

/*
 * Привязка позиции прокрутки к другой части. Значение только ХРАНИТСЯ — см.
 * комментарий у `scroll_pos_x_link` в parts_internal.h. Одноразовый WARNING на
 * непустую привязку: допущение должно быть видно, а не прятаться за тихим
 * дефолтом (тот же приём, что у `クリップ領域`).
 */
void PE_set_component_scroll_pos_link(int parts_no, int link_parts_no, bool vertical)
{
	struct parts *parts = parts_get(parts_no);
	if (vertical)
		parts->scroll_pos_y_link = link_parts_no;
	else
		parts->scroll_pos_x_link = link_parts_no;
	if (link_parts_no) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("привязка прокрутки (часть %d -> %d): значение сохранено, "
				"само следование не реализовано", parts_no, link_parts_no);
		}
	}
}

void PE_set_on_cursor_show_link(int parts_no, int target_parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return;
	parts->on_cursor_show_link = target_parts_no;
	// До первого наведения подсказки быть не должно: `表示 = 1` в раскладке значит
	// «показать, когда позовут», а зовёт её именно курсор.
	parts_set_show(parts, false);
}

int PE_get_component_scroll_pos_link(int parts_no, bool vertical)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return 0;
	return vertical ? parts->scroll_pos_y_link : parts->scroll_pos_x_link;
}

void PE_parts_set_lock_input_state(int parts_no, bool lock)
{
	parts_get(parts_no)->lock_input_state = lock;
}

bool PE_init_parts_movie(int parts_no, int width, int height, int bg_r, int bg_g, int bg_b, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_movie *movie = parts_get_movie(parts, state);
	// Обратный случай к PE_create_parts_movie: на части мог висеть ролик APEG.
	if (movie->apeg) {
		apeg_movie_close(movie->apeg);
		movie->apeg = NULL;
		gfx_delete_texture(&movie->common.texture);
	}

	int sp_no = sact_SP_GetUnuseNum(0);
	struct sact_sprite *sp = sact_create_sprite(sp_no, width, height, bg_r, bg_g, bg_b, 255);
	if (!sp)
		return false;
	// The movie frames are composited by the parts engine via parts_render(),
	// so hide the bound sprite from the scene to avoid double-drawing.
	sprite_set_show(sp, false);

	struct texture *tex = sprite_get_texture(sp);
	movie->sprite_no = sp_no;
	movie->common.texture = *tex; // XXX: textures normally shouldn't be copied like this...
	parts_set_dims(parts, &movie->common, width, height);
	parts_dirty(parts);
	return true;
}

/*
 * Новое API movie-частей (Ixseal): ролик APEG принадлежит самой части.
 * `CreatePartsMovie(Number, FileName, SoundID, SoundGroup, Red, Green, Blue, State)` —
 * ★цвет фона приходит НЕ в RGB, а в YCbCr: игра переводит его сама
 * (`movie::detail::CDrawMovie@SetBackColor`), а движок хранит кадр в YCbCr.
 */
bool PE_create_parts_movie(int parts_no, struct string *filename, int sound_id, int sound_group,
			   int back_y, int back_cb, int back_cr, int state)
{
	(void)sound_id;   // номер звука движку не нужен: звук лежит внутри ролика
	if (!parts_state_valid(--state) || !filename)
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_movie *movie = parts_get_movie(parts, state);
	if (movie->apeg) {
		apeg_movie_close(movie->apeg);
		movie->apeg = NULL;
		gfx_delete_texture(&movie->common.texture);
	}
	// ★Часть могла до этого обслуживать СТАРОЕ API (InitPartsMovie): текстура там
	// принадлежит спрайту SACT. Не отпустив спрайт, мы утекли бы им, а потом снесли
	// бы чужую текстуру как свою.
	if (movie->sprite_no >= 0) {
		sact_SP_Delete(movie->sprite_no);
		movie->sprite_no = -1;
		movie->common.texture.handle = 0;
	}

	struct apeg_movie *m = apeg_movie_open(filename->text, sound_group, back_y, back_cb, back_cr);
	if (!m)
		return false;
	movie->apeg = m;
	int w = apeg_movie_width(m), h = apeg_movie_height(m);
	gfx_init_texture_with_pixels(&movie->common.texture, w, h,
				     (void *)apeg_movie_pixels(m));
	// Размер части игра читает сразу после создания (IParts@Size::get), поэтому
	// он должен встать здесь, из заголовка ролика.
	parts_set_dims(parts, &movie->common, w, h);
	parts_dirty(parts);
	return true;
}

static struct parts_movie *parts_try_get_movie(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return NULL;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_MOVIE)
		return NULL;
	return parts->states[state].movie.apeg ? &parts->states[state].movie : NULL;
}

bool PE_release_parts_movie(int parts_no, int state)
{
	struct parts_movie *movie = parts_try_get_movie(parts_no, state);
	if (!movie)
		return false;
	apeg_movie_close(movie->apeg);
	movie->apeg = NULL;
	gfx_delete_texture(&movie->common.texture);
	return true;
}

bool PE_play_parts_movie(int parts_no, int msec, int state)
{
	struct parts_movie *movie = parts_try_get_movie(parts_no, state);
	if (!movie)
		return false;
	return apeg_movie_play(movie->apeg, msec);
}

void PE_set_movie_time(int parts_no, int msec, int state)
{
	struct parts_movie *movie = parts_try_get_movie(parts_no, state);
	if (movie)
		apeg_movie_set_time(movie->apeg, msec);
}

bool PE_is_end_parts_movie(int parts_no, int state)
{
	struct parts_movie *movie = parts_try_get_movie(parts_no, state);
	// Ролика нет — считаем, что он уже кончился: иначе игра повиснет в ожидании.
	return movie ? apeg_movie_is_end(movie->apeg) : true;
}

int PE_get_parts_movie_end_time(int parts_no, int state)
{
	struct parts_movie *movie = parts_try_get_movie(parts_no, state);
	return movie ? apeg_movie_end_time(movie->apeg) : 0;
}

int PE_get_parts_movie_current_time(int parts_no, int state)
{
	struct parts_movie *movie = parts_try_get_movie(parts_no, state);
	return movie ? apeg_movie_current_time(movie->apeg) : 0;
}

int PE_get_movie_sprite(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return -1;

	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_MOVIE)
		return -1;

	return parts->states[state].movie.sprite_no;
}

bool PE_CreateParts3DLayerPluginID(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts *parts = parts_get(parts_no);
	struct parts_3dlayer *l = parts_get_3dlayer(parts, state);

	if (l->plugin >= 0)
		return false;

	int handle = ReignEngine_create_plugin(RE_SEAL_PLUGIN);
	if (handle < 0)
		return false;

	int sp_no = sact_SP_GetUnuseNum(0);
	struct sact_sprite *sp = sact_create_sprite(sp_no, 1, 1, 0, 0, 0, 255);
	if (!sp) {
		ReignEngine_ReleasePlugin(handle);
		return false;
	}

	if (!ReignEngine_BindPlugin(handle, sp_no)) {
		sact_SP_Delete(sp_no);
		ReignEngine_ReleasePlugin(handle);
		return false;
	}
	// The 3D content is composited by the parts engine via parts_render(), so
	// hide the bound sprite from the scene to avoid double-drawing.
	sprite_set_show(sp, false);

	l->plugin = handle;
	l->sprite_no = sp_no;
	return true;
}

int PE_GetParts3DLayerPluginID(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return -1;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_3DLAYER)
		return -1;
	return parts->states[state].layer3d.plugin;
}

bool PE_ReleaseParts3DLayerPluginID(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_3DLAYER)
		return false;
	struct parts_3dlayer *l = &parts->states[state].layer3d;
	if (l->plugin < 0)
		return false;
	ReignEngine_ReleasePlugin(l->plugin);
	l->plugin = -1;
	if (l->sprite_no >= 0) {
		sact_SP_Delete(l->sprite_no);
		l->sprite_no = -1;
	}
	return true;
}

/*
 * Часть ПЕРЕЖИЛА свой слой, и игра к ней снова обратилась — переносим её вместе
 * с поддеревом на слой, который сейчас активен.
 *
 * Зачем: у частей мёртвого слоя ключ сортировки — 0 (см. parts_get_sprite_z),
 * то есть они уходят ПОД ВСЁ и на экране их не видно. Для брошенного экрана это
 * ровно то, что нужно, а вот окно реплик ADV игра переиспользует между сценами:
 * номера частей она помнит с пролога и продолжает слать в них
 * SetMessageWindowText. Симптом — диалог после фазы Hustling: имя персонажа и
 * кнопки есть, реплики нет; замер показывал, что игра текст ПОДАЁТ
 * (`MWTEXT part=90000088 msg=101 raw='О, привет, Порно…'`), а часть висит на
 * `ctrl=4` при стеке `0 19`.
 *
 * Переносим ТОЛЬКО по явному обращению — те части, которых игра больше не
 * трогает, так и остаются под всем.
 */
void parts_adopt_to_active_layer(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return;
	// ★СИСТЕМНЫЙ ОВЕРЛЕЙ — НЕ «МЁРТВЫЙ СЛОЙ»: позиции в стеке у него нет по
	// устройству, он рисуется поверх всего. Утащить с него часть значит вернуть
	// баг, ради которого её туда и положили (баннер «сохранено» уезжал ПОД экран
	// SAVE). Проверка нужна с тех пор, как усыновление зовётся ещё и из SetShow.
	if (parts->controller_no == PARTS_CONTROLLER_SYSTEM_OVERLAY)
		return;
	if (ctrl_stack_pos(parts->controller_no) >= 0)
		return;   // слой жив — трогать нечего
	int active = ctrl_stack.active;
	if (ctrl_stack_pos(active) < 0)
		return;
	if (getenv("XSYS4_ADOPT_TRACE"))
		NOTICE("ADOPT part=%d: слой %d мёртв -> %d", parts_no,
		       parts->controller_no, active);
	/*
	 * Поддерево целиком: у окна реплик это подложка, текст и маркер «дальше».
	 * ★Сначала СОБИРАЕМ, потом переносим: parts_list_resort переставляет часть в
	 * том же списке, по которому идёт обход, и правка на лету уводила итерацию —
	 * окно переезжало, а его текст оставался на мёртвом слое (видно дампом:
	 * `90000088 ctrl=19`, а `90000089 ctrl=4`).
	 */
	int *moved = NULL;
	int nr_moved = 0;
	struct parts *p;
	PARTS_LIST_FOREACH(p) {
		for (struct parts *anc = p; anc; anc = anc->parent) {
			if (anc->no != parts_no)
				continue;
			moved = xrealloc_array(moved, nr_moved, nr_moved + 1, sizeof(int));
			moved[nr_moved++] = p->no;
			break;
		}
	}
	for (int i = 0; i < nr_moved; i++) {
		struct parts *q = parts_try_get(moved[i]);
		if (!q)
			continue;
		q->controller_no = active;
		parts_list_resort(q);
	}
	free(moved);
	parts->controller_no = active;
	parts_list_resort(parts);
}
