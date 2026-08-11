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

#include "system4.h"
#include "system4/string.h"

#include "asset_manager.h"
#include "audio.h"
#include "input.h"
#include "xsystem4.h"
#include "parts.h"
#include "parts_internal.h"
#include "vm.h"

// true while at least one BeginClick/BeginInput session is open
bool parts_began_click = false;
// Nesting depth of those sessions. A modal child screen opens its own session on
// top of its parent's: Tsumamigui 3's save screen calls BeginInput, then the
// "save here?" dialog calls BeginInput again, and closing the dialog calls
// EndInput ONCE — the parent never ends its own session. With a plain flag that
// single EndInput killed input for the whole game: parts stopped updating their
// hover/click state, so the save screen no longer answered Parts_GetInputState
// and neither RETURN nor any slot responded (looked like a hang; the frame loop
// was still running). Counting sessions keeps the parent's alive.
static int parts_input_depth = 0;

// the mouse position at last update
static Point parts_prev_pos = {0};
// true when the left mouse button was down last update
static bool prev_clicking = false;
// the last (fully) clicked parts number
static int clicked_parts = 0;
// the current (partially) clicked parts number
static int click_down_parts = 0;

// drag state
static struct parts *drag_parts = NULL;
static bool is_dragging = false;
// scrollbar (config slider) currently being dragged, if any
static struct parts *dragging_scrollbar = NULL;
static Point drag_initial_pos;
static Point drag_start_cursor;
static struct parts *drop_target = NULL;

static bool parts_pixel_hittest(struct parts_common *c, Rectangle hitbox, Point pos);

static bool parts_hittest(struct parts *parts, int state, Point pos)
{
	struct parts_common *c = &parts->states[state].common;
	float sx = parts->global.scale.x, sy = parts->global.scale.y;
	Rectangle hitbox;
	if (sx != 1.0f || sy != 1.0f) {
		// Scaled parts render as global.pos + scale·origin_offset, size scale·wh.
		// The stored hitbox is unscaled, so recompute it here (e.g. the config
		// message-window sample is drawn at 0.5× — its hit area must match, or
		// its full-size box would swallow clicks meant for the buttons behind).
		hitbox = (Rectangle){
			parts->global.pos.x + (int)(c->origin_offset.x * sx),
			parts->global.pos.y + (int)(c->origin_offset.y * sy),
			(int)(c->w * sx),
			(int)(c->h * sy),
		};
	} else {
		hitbox = c->hitbox;
		if (parts->parent) {
			hitbox.x += parts->parent->global.pos.x;
			hitbox.y += parts->parent->global.pos.y;
		}
	}
	if (!SDL_PointInRect(&pos, &hitbox))
		return false;
	// `ＣＧ判定パーツ` — это и есть «область по форме картинки»: попиксельная
	// проверка для него не опция раскладки, а сам смысл типа.
	if (!parts->pixel_hittest
	    && parts->states[parts->state].type != PARTS_CG_DETECTION)
		return true;
	return parts_pixel_hittest(c, hitbox, pos);
}

// `マウスカーソルピクセル判定`: попадание считается только по НЕПРОЗРАЧНОМУ пикселю
// текстуры. Альфа читается с GPU ОДИН раз на состояние (glReadPixels синхронизирует
// конвейер, а hit-тест идёт каждый кадр) и кэшируется в common->hit_mask.
static bool parts_pixel_hittest(struct parts_common *c, Rectangle hitbox, Point pos)
{
	if (!c->texture.handle || c->texture.w <= 0 || c->texture.h <= 0)
		return true;  // нечего проверять — остаётся прямоугольник
	if (!c->hit_mask) {
		uint8_t *px = gfx_get_pixels(&c->texture);
		if (!px)
			return true;
		int n = c->texture.w * c->texture.h;
		c->hit_mask = xmalloc(n);
		for (int i = 0; i < n; i++)
			c->hit_mask[i] = px[i * 4 + 3];
		c->hit_mask_w = c->texture.w;
		c->hit_mask_h = c->texture.h;
		free(px);
	}
	// Точка → тексель: hitbox растянут на всю текстуру (при масштабе — с ним).
	if (hitbox.w <= 0 || hitbox.h <= 0)
		return true;
	int tx = (int)((int64_t)(pos.x - hitbox.x) * c->hit_mask_w / hitbox.w);
	int ty = (int)((int64_t)(pos.y - hitbox.y) * c->hit_mask_h / hitbox.h);
	if (tx < 0 || ty < 0 || tx >= c->hit_mask_w || ty >= c->hit_mask_h)
		return false;
	return c->hit_mask[ty * c->hit_mask_w + tx] != 0;
}

static void drag_state_reset(void)
{
	drag_parts = NULL;
	is_dragging = false;
	drop_target = NULL;
}

void parts_input_reset_drag(struct parts *parts)
{
	if (parts == drag_parts)
		drag_state_reset();
	else if (parts == drop_target)
		drop_target = NULL;
	if (parts == dragging_scrollbar)
		dragging_scrollbar = NULL;
}

/*
 * Может ли часть В ПРИНЦИПЕ получить ввод. Чистая ДЕКОРАЦИЯ не должна перехватывать
 * курсор у кнопок под собой.
 *
 * Повод: титул Haha Ranman. Поверх меню (части z 5..20) висит полноэкранная плёночная
 * «грязь» 1000001057 — CG 1280×720, НЕПРОЗРАЧНАЯ (alpha 220..255, так что попиксельная
 * проверка не спасает), `pass_cursor = 0`, и она МЕДЛЕННО ЕДЕТ. На кадрах, где её
 * прямоугольник накрывал пункт, она забирала курсор, на остальных — нет: в трейсе
 * `MOUSE_ENTER`/`MOUSE_LEAVE` чередовались КАЖДЫЙ КАДР у пункта 90000026 и у неё.
 * Клик срабатывал только если случайно попадал в «хороший» кадр — один раз из двух.
 *
 * Гейт узкий и опирается на доказуемый факт: в НОВОМ message-API (тот, где объявлена
 * `PartsEngine.SeekMessage`) диспатчер игры
 * `parts::detail::CPartsMessageManager@CallDelegate` при `delegateIndex < 0` сразу
 * возвращает 0 — часть без набора обработчиков не может получить НИ ОДНОГО сообщения,
 * поэтому перехват курсора ею — чистая потеря. Старые игры (v6/v7) диспатчат по НОМЕРУ
 * части и `delegate_index` не выставляют вовсе, поэтому там правило не применяется.
 *
 * Типы-детекторы (`ＣＧ判定パーツ`, `矩形パーツ`) и всё, что несёт собственную механику
 * ввода (кнопка, чекбокс, скроллбар, перетаскивание, колесо, попиксельный хит),
 * считаются способными к вводу всегда — независимо от делегата.
 */
static bool parts_can_take_cursor(struct parts *parts)
{
	// ★`wheelable` СЮДА НЕ ГОДИТСЯ: его дефолт — true у КАЖДОЙ части (parts_init),
	// то есть признаком способности к вводу он не является и обнулял весь гейт.
	bool own_input = parts->clickable > 0 || parts->is_button || parts->pixel_hittest
			|| parts->is_checkbox || parts->is_hscrollbar || parts->is_vscrollbar
			|| parts->draggable;
	enum parts_type t = parts->states[parts->state].type;
	bool detector = (t == PARTS_CG_DETECTION || t == PARTS_RECT_DETECTION);
	bool r = own_input || detector || !parts_msg_api_new() || parts->delegate_index >= 0;
	if (getenv("XSYS4_CURSOR_TRACE"))
		NOTICE("CURSORTRACE part=%d -> %d (own=%d det=%d type=%d dg=%d newapi=%d"
		       " clk=%d btn=%d px=%d chk=%d hsb=%d vsb=%d drag=%d whl=%d)",
		       parts->no, r, own_input, detector, t, parts->delegate_index,
		       parts_msg_api_new(), parts->clickable, parts->is_button,
		       parts->pixel_hittest, parts->is_checkbox, parts->is_hscrollbar,
		       parts->is_vscrollbar, parts->draggable, parts->wheelable);
	return r;
}

/*
 * Звук наведения/клика части: НЕ ЗАДАН — это −1 (`parts_init` в `parts.c`), и такие
 * поля у Tsumamigui 3 почти у всех частей (`オンカーソル効果音 = ""` в раскладках).
 * Прежде номер уходил в `audio_play_sound` без проверки, тот доходил до
 * `channel_open(ASSET_SOUND, -1)` и печатал `Failed to load WAV -1` на КАЖДОЕ
 * наведение — 368 предупреждений за живой прогон пользователя. Помимо шума это
 * лишний поиск в архиве на каждый ховер и, что хуже, маскировка НАСТОЯЩИХ
 * «Failed to load WAV N» в логе. Та же проверка `>= 0` уже стоит в `parts/debug.c`.
 */
static void parts_play_sound(int sound_no)
{
	if (sound_no < 0)
		return;
	// XSYS4_SND_TRACE=1 — единственный способ увидеть звук части замером: слышимость
	// на стенде не проверить (SDL_AUDIODRIVER=dummy), а `audio_play_sound` возвращает
	// false и при неудачной загрузке, и при нехватке каналов.
	bool ok = audio_play_sound(sound_no);
	if (getenv("XSYS4_SND_TRACE"))
		NOTICE("PARTSND играем звук %d -> %s", sound_no, ok ? "ок" : "НЕ ВЫШЛО");
}

static void parts_update_mouse(struct parts *parts, Point cur_pos, bool cur_clicking,
		int passed_time, bool *hover_consumed, bool *click_consumed)
{
	// SetEnableInputProcess(false) выключает часть из обработки ввода целиком: ни
	// hit-теста, ни сообщений, и курсор она НЕ перехватывает у частей за собой.
	// Сбрасываем is_hovered, иначе «залипший» hover выстрелит MOUSE_LEAVE позже.
	// XSYS4_PART_WATCH=<номер>: весь путь решения по вводу для этой части — раз в
	// кадр, вместе с вычисленным hit-тестом. Общий XSYS4_CURSOR_TRACE упирается в
	// лимит лога задолго до интересного места (200k строк — до открытия конфига).
	if (parts_watched(parts->no)) {
		NOTICE("INWATCH part=%d eip=%d can=%d show=%d alpha=%d lhid=%d hit=%d pos=%d,%d",
		       parts->no, parts->enable_input_process, parts_can_take_cursor(parts),
		       parts->global.show, parts->global.alpha, parts_hidden_by_layer(parts),
		       parts_hittest(parts, PARTS_STATE_DEFAULT, cur_pos),
		       cur_pos.x, cur_pos.y);
	}
	if (!parts->enable_input_process) {
		parts->is_hovered = false;
		return;
	}
	// Чистая декорация — так же вне обработки ввода (см. parts_can_take_cursor).
	if (!parts_can_take_cursor(parts)) {
		parts->is_hovered = false;
		return;
	}
	// Always use DEFAULT state hitbox regardless of the current display state.
	// ПОГАШЕННАЯ часть (`表示 = 0`) во вводе не участвует — то же условие, что и в
	// рендере (`if (!parts->global.show) return`), и курсор она не перехватывает у
	// частей за собой. Ловушка, стоившая ЦЕЛИКОМ экранов SAVE и LOAD: активность
	// `セーブロード` держит поверх всего невидимую полноэкранную область-детектор
	// (`矩形パーツ`, часть 90000032, z=18, 1024×768) и объявляет её `表示 = 0`. Пока
	// загрузчик активностей не умел разбирать прямоугольное состояние, часть была
	// пустышкой 0×0 и вреда не делала; как только научился, она стала съедать КАЖДЫЙ
	// клик и hover этих экранов — слоты, страницы и `もどる` перестали отвечать, а
	// сохранение молча не происходило. Различать надо именно по `show`, а не по типу:
	// ВИДИМАЯ (`表示 = 1`) область-детектор ничего не рисует, но курсор ловит — в этом
	// весь смысл типа.
	// Парты погашенного СЛОЯ невидимы по той же причине: пока открыта бэк-сцена,
	// экран игры спрятан целиком (SetComponentShow с ID контроллера), и его кнопки не
	// должны перехватывать клики у вьювера.
	// ПОИМЁННО погашенная часть (`SetShow(0)`) — тоже: раз её не рисует render_parts,
	// она не может и перехватывать курсор. Без этого условия на титуле Dohna НИ ОДНА
	// кнопка не получала hover: над экраном висят полноэкранные (1280x720) части
	// перехода 1000001031/1000001032 с `z = INT_MAX`/`INT_MAX-1` и `表示 = 0`; первая
	// же из них в обходе front-to-back давала hit и ставила hover_consumed, съедая
	// курсор у всего экрана (clicked_parts не становился ненулевым никогда).
	// Непостроенный `構築パーツ` (construction_mask) render_parts тоже пропускает: он
	// служит только прямоугольной маской альфа-клиппера, а по своей же процедуре
	// построения его поверхность ПРОЗРАЧНА (см. construction_mask в parts_internal.h).
	// Прозрачную маску курсор обязан проходить насквозь: на титуле Dohna такая часть
	// (90000041, `表示 = 1`, 1480x920 при экране 1280x720, z поверх всего меню) была
	// вторым слоем той же поломки — она перехватывала курсор у кнопок уже ПОСЛЕ того,
	// как перестали мешать погашенные части перехода.
	// Полностью прозрачная часть (`アルファ = 0`) невидима — и курсор проходит сквозь неё.
	// Третий слой той же поломки на титуле Dohna: часть `Overlay` (90000040, CG
	// `システム／タイトル／ぼかし` — размывка фона под всплывающими экранами) объявлена
	// `表示 = 1`, но `アルファ = 0`, и полноэкранная (1280x720) поверх всего меню.
	// Проверяем ТЕКУЩУЮ alpha, а не объявленную: игра проявляет размывку motion-фейдом,
	// и как только она станет видимой, то и курсор обязана перехватывать.
	bool is_hovered = parts->global.show
		&& parts->global.alpha > 0
		&& !parts->construction_mask
		&& !parts_hidden_by_layer(parts)
		&& parts_hittest(parts, PARTS_STATE_DEFAULT, cur_pos)
		&& !*hover_consumed;

	bool was_hovered = parts->is_hovered;
	parts->is_hovered = is_hovered;

	// Вторая половина INWATCH: итог и был ли курсор уже съеден кем-то выше.
	if (parts_watched(parts->no))
		NOTICE("INWATCH2 part=%d hovered=%d consumed_before=%d",
		       parts->no, (int)is_hovered, (int)*hover_consumed);
	// Кто съел курсор над наблюдаемой точкой: печатаем ЛЮБОЙ парт, который стал
	// hovered и потребил курсор в кадре, где включён XSYS4_HOVER_TRACE.
	if (getenv("XSYS4_HOVER_TRACE") && is_hovered && !parts->pass_cursor)
		NOTICE("HOVER consumed by part=%d z=%d ctrl=%d", parts->no,
		       parts->global.z, parts->controller_no);

	// !pass_cursor parts consume the cursor for parts behind them
	if (is_hovered && !parts->pass_cursor)
		*hover_consumed = true;

	if (parts->linked_from >= 0 && is_hovered != was_hovered) {
		parts_dirty(parts_get(parts->linked_from));
	}

	if (!parts_began_click)
		return;

	if (is_hovered && !was_hovered) {
		parts_msg_push(parts, PARTS_MSG_MOUSE_ENTER, "ii", cur_pos.x, cur_pos.y);
		parts->hover_time = 0;
	}
	if (!is_hovered && was_hovered) {
		parts_msg_push(parts, PARTS_MSG_MOUSE_LEAVE, "ii", cur_pos.x, cur_pos.y);
	}

	if (is_hovered) {
		// MOUSE_ON message fires every frame while hovering
		parts_msg_push(parts, PARTS_MSG_MOUSE_ON,
				"iii", cur_pos.x, cur_pos.y, parts->hover_time);
		parts->hover_time += passed_time;

		// MOUSE_MOVE message fires when cursor moves while hovering
		if (cur_pos.x != parts_prev_pos.x || cur_pos.y != parts_prev_pos.y) {
			parts_msg_push(parts, PARTS_MSG_MOUSE_MOVE, "ii", cur_pos.x, cur_pos.y);
		}
	}

	if (!is_hovered) {
		parts_set_state(parts, PARTS_STATE_DEFAULT);
		return;
	}

	/*
	 * ЯВНЫЙ ЗАПРЕТ игры сильнее любого фолбэка: `clickable == −1` — это
	 * `SetClickable(false)`, а чаще `SetButtonEnable(false)`, которым игра
	 * ГАСИТ кнопку (тот же вызов подменяет её CG на `／無効`).
	 *
	 * Живой случай — домашняя сцена Dohna: плитки `Squad` и `Shop` до открытия
	 * по сюжету нарисованы серыми (CG `システム／アジト／ボタン／メンバー／無効`,
	 * `／ショップ／無効`), и в оригинале они НЕ НАЖИМАЮТСЯ. У нас клик проходил
	 * насквозь — игра уходила в `SceneAzito@OpenParty`, то есть в экраны, до
	 * которых игрок дойти не должен; там и словили падение на
	 * `Parts_SetHGaugeReverse`. В дампе частей видно ровно это: у серых плиток
	 * `clk=0 btn=1`, у живых `Garage`/`Talent` — `clk=1 btn=1`, значит запрет
	 * до движка доходил и терялся уже здесь.
	 */
	// Погашенная кнопка не подсвечивается и не звучит под курсором — но курсор
	// у частей за собой всё же перехватывает: она непрозрачная и лежит поверх.
	if (parts->clickable < 0) {
		parts_set_state(parts, PARTS_STATE_DEFAULT);
		return;
	}
	bool click_eligible = parts->clickable > 0 || !parts->pass_cursor;
	if (!click_eligible || *click_consumed) {
		if (!was_hovered)
			parts_play_sound(parts->on_cursor_sound);
		parts_set_state(parts, PARTS_STATE_HOVERED);
		return;
	}

	// click down: first eligible part captures the click
	if (cur_clicking && !prev_clicking) {
		click_down_parts = parts->no;
		*click_consumed = true;

		drag_parts = parts;
		drag_initial_pos = parts->local.pos;
		drag_start_cursor = cur_pos;

		// KEY_TRIGGER — момент нажатия. KEY_PRESS (16) шлём тем же кадром: на него игра
		// вешает кнопки-стрелки скроллбара (AddKeyPressEvent), и одно нажатие должно
		// давать один шаг. Отдельно от KEY_DOWN, который идёт каждый кадр удержания —
		// на нём листание было бы бесконечным.
		parts_msg_push(parts, PARTS_MSG_KEY_TRIGGER, "i", VK_LBUTTON);
		parts_msg_push(parts, PARTS_MSG_KEY_PRESS, "i", VK_LBUTTON);
	}

	// KEY_DOWN message fires every frame while held (not first frame)
	if (prev_clicking && cur_clicking && click_down_parts == parts->no) {
		parts_msg_push(parts, PARTS_MSG_KEY_DOWN, "i", VK_LBUTTON);
	}

	if (cur_clicking && click_down_parts == parts->no) {
		parts_set_state(parts, PARTS_STATE_CLICKED);
	} else {
		if (!was_hovered) {
			parts_play_sound(parts->on_cursor_sound);
		}
		parts_set_state(parts, PARTS_STATE_HOVERED);
	}

	// click event: only if the click down event had same parts number.
	// Условие — та же click_eligible, что и у нажатия выше, а НЕ один parts->clickable.
	// `clickable` выставляет только SACT-овский PartsEngine.SetClickable, которого новые
	// игры не зовут: Tsumamigui 3 держит карту обработчиков у себя
	// (`CPartsMessageManager@AddMouseLClickEvent`) и ждёт от движка сообщение MOUSE_CLICK.
	// Из-за лишнего условия MOUSE_CLICK и KEY_UP не отправлялись НИ ОДНОМУ парту за всю
	// сессию (проверено трейсом: `clicked_parts` не становился ненулевым никогда), поэтому
	// кнопки, повешенные игрой на клик, молчали — например 戻る в BACK SCENE (парт 90000021):
	// нажатие доходило (KEY_TRIGGER), клик — нет, и вьювер невозможно было закрыть.
	if (click_eligible && prev_clicking && !cur_clicking
			&& click_down_parts == parts->no) {
		parts_play_sound(parts->on_click_sound);
		// XSYS4_CLICKNO_TRACE=1 — ЖИЗНЬ «номера кликнутой части». На нём стоит выход
		// из ожидания клика у игры (`parts::detail::WaitForClick` крутится, пока
		// `GetClickNumber() <= 0`), поэтому лишний ненулевой номер = лишний
		// пролистанная реплика. Печатаем и установку, и обнуление, и глубину ввода.
		// ★С ИМЕНЕМ КАРТИНКИ: по одному номеру части не понять, какую кнопку нажали, а
		// по логу нужно уметь восстановить действия ЗАДНИМ ЧИСЛОМ — падение уносит
		// процесс, и разового дампа по сигналу уже не снять.
		if (getenv("XSYS4_CLICKNO_TRACE")) {
			struct parts_state *st = &parts->states[parts->state];
			const char *cg = (st->type == PARTS_CG && st->cg.name)
			                 ? display_sjis0(st->cg.name->text) : "";
			NOTICE("CLICKNO часть %d кликнута (глубина ввода %d) cg=\"%s\"",
			       parts->no, parts_input_depth, cg);
		}
		clicked_parts = parts->no;

		// Checkbox: flip state on click; the game reads it via IsCheckBoxChecked.
		// ★И СРАЗУ СООБЩАЕМ ИГРЕ: одного локального переключения мало — вся логика
		// висит на сообщении CHANGED_FLG (см. parts_internal.h). Шлём ТОЛЬКО когда
		// состояние действительно изменилось: у недоступного чекбокса toggle
		// возвращает false, и события быть не должно.
		if (parts->is_checkbox && parts_checkbox_toggle(parts)) {
			parts_msg_push(parts, PARTS_MSG_CHANGED_FLG, "i",
					parts->checkbox_checked ? 1 : 0);
			/*
			 * Кнопка ГРУППЫ радиокнопок объявляется ДВАЖДЫ, и это не
			 * перестраховка, а замер: страницы конфига подписаны по-разному.
			 * `ウィンドウ` вешает обработчик на ГРУППУ
			 * (`GetRadioButtonBox(...).ChangedEvent`) и читает `Checked` у её
			 * кнопок сам; `入力` вешает его на САМИ КНОПКИ, как на обычные
			 * чекбоксы (`SetMoveMouseCursorSpeed`, `SetWheelForward` приходят
			 * только оттуда). Аргумент события группы — индекс выбранной кнопки.
			 */
			int idx = 0;
			int box_no = parts_radio_box_number(parts->no, &idx);
			if (box_no >= 0) {
				struct parts *box = parts_try_get(box_no);
				if (box)
					parts_msg_push(box, PARTS_MSG_CHANGED, "i", idx);
			}
		}

		// Текстовое поле ввода забирает фокус клавиатуры по клику (и отдаёт
		// его, если кликнули мимо) — как в оригинале, где каретка видна ровно
		// в том поле, куда идёт ввод.
		PE_textbox_click(parts->no);

		parts_msg_push(parts, PARTS_MSG_MOUSE_CLICK,
				"iii", cur_pos.x, cur_pos.y, VK_LBUTTON);
		parts_msg_push(parts, PARTS_MSG_KEY_UP, "i", VK_LBUTTON);
	}
}

/*
 * `SetEnableInput`/`IsEnableInput` — ГЛОБАЛЬНЫЙ гейт ввода партов (в отличие от
 * пер-партового `SetEnableInputProcess`). Игра выключает его, пока идёт то, во что
 * нельзя вмешиваться — переход между экранами, анимация, промотка, — и включает
 * обратно; сама же читает его назад, поэтому значение надо ХРАНИТЬ, а не только
 * учитывать.
 *
 * Пока функций не было, Haha Ranman валилась в debug-REPL, и запускалась только
 * костылём `XSYS4_LENIENT_HLL=1` (заглушка отдавала 0 = «ввод выключен», что для
 * `IsEnableInput` ещё и врёт).
 */
static bool parts_input_enabled = true;

void PE_SetEnableInput(bool enable)
{
	parts_input_enabled = !!enable;
	if (getenv("XSYS4_INPUT_TRACE"))
		NOTICE("INPUT SetEnableInput(%d)", (int)parts_input_enabled);
}

bool PE_IsEnableInput(void)
{
	return parts_input_enabled;
}

void PE_UpdateInputState(int passed_time)
{
	Point cur_pos;
	bool cur_clicking = key_is_down(VK_LBUTTON);
	mouse_get_pos(&cur_pos.x, &cur_pos.y);

	/*
	 * ★ДИАГНОСТИКА — ДО ГЕЙТА. Разовый дамп по kill -USR1 нужен ровно тогда, когда
	 * «ничего не нажимается», а это чаще всего и означает выключенный гейт: стоя
	 * ниже, печать молчала именно в том случае, ради которого её завели. Заодно
	 * печатаем само состояние гейта — по одной строке видно, ввод выключен или
	 * перехвачен какой-то частью.
	 */
	if (parts_take_debug_dump_request()) {
		NOTICE("--- PARTS DUMP (по сигналу) --- ввод %s, курсор %d,%d, кнопка %d",
		       parts_input_enabled ? "ВКЛЮЧЁН" : "ВЫКЛЮЧЕН (SetEnableInput(false))",
		       cur_pos.x, cur_pos.y, (int)cur_clicking);
		parts_debug_dump();
	}

	// Ввод выключен игрой — не наводим, не кликаем и не рассылаем сообщения.
	// Позицию курсора при этом всё равно прочли: игра её опрашивает отдельно.
	if (!parts_input_enabled)
		return;

	if (getenv("XSYS4_INPUT_TRACE")) {
		static int ncalls = 0;
		if ((ncalls++ % 30) == 0 || cur_clicking || clicked_parts)
			NOTICE("INPUT UpdateInputState #%d: clicking=%d pos=%d,%d clicked_parts=%d",
			       ncalls, cur_clicking, cur_pos.x, cur_pos.y, clicked_parts);
	}
	if (getenv("XSYS4_DUMP_PARTS")) {
		static int dcnt = 0;
		if ((dcnt++ % 120) == 5) {
			NOTICE("--- PARTS DUMP (live) ---");
			parts_debug_dump();
		}
	}

	/*
	 * Редактирующие клавиши текстового поля. Печатные символы приходят полю
	 * событием SDL_TEXTINPUT (register_input_handler), а Backspace/Delete/стрелки
	 * текстом не приходят вовсе — их приходится опрашивать здесь. Реагируем на
	 * ФРОНТ нажатия, иначе одно нажатие съело бы всю строку за несколько кадров.
	 */
	{
		// RETURN здесь же: им поле подтверждает ввод (сообщение FIXED).
		static const int edit_keys[] = { VK_BACK, VK_DELETE, VK_LEFT, VK_RIGHT, VK_RETURN };
		static bool was_down[5];
		for (int i = 0; i < 5; i++) {
			bool down = key_is_down(edit_keys[i]);
			if (down && !was_down[i])
				PE_textbox_key(edit_keys[i]);
			was_down[i] = down;
		}
	}

	// ★ПЕРЕД ОБХОДОМ — сверяем порядок списка с z: если он устарел, курсор достанется
	// не той части (см. parts_list_order_check).
	parts_list_order_check();

	bool hover_consumed = false;
	bool click_consumed = false;
	struct parts *parts;
	// Iterate front-to-back (highest z first) for proper cursor consumption
	PARTS_LIST_FOREACH_REVERSE(parts) {
		parts_update_mouse(parts, cur_pos, cur_clicking, passed_time,
				&hover_consumed, &click_consumed);
	}

	/*
	 * `オンカーソル表示連動`: часть видна ровно пока курсор над ТОЙ, к которой её
	 * привязала раскладка (имя цели разрешает загрузчик — см. on_cursor_show_link).
	 * Отдельным проходом, ПОСЛЕ основного: hover цели считается в том же цикле, и
	 * порядок front-to-back не даёт узнать её состояние заранее. Так на странице
	 * `Window` конфига Dohna появляется одна подсказка — та, на пункт которой навели,
	 * а не все четыре разом.
	 */
	PARTS_LIST_FOREACH(parts) {
		if (parts->on_cursor_show_link < 0)
			continue;
		struct parts *target = parts_try_get(parts->on_cursor_show_link);
		parts_set_show(parts, target && target->is_hovered);
	}

	// Mouse wheel: deliver a MOUSE_WHEEL message (Forward/Back counts) to hovered
	// parts. The engine never generated these, so wheel-scrollable UIs (Tsumamigui 3
	// BACK LOG: a full-screen part catches the notch -> opens the log; inside the
	// log CBackLogView@MouseWheelEvent -> scrollbar pos -> ScrollEvent -> SetLineIndex)
	// could not scroll. Broadcast to every hovered part and let the game's message
	// manager dispatch to whichever registered a handler.
	//
	// We consume the PARTS wheel accumulator (mouse_get/clear_parts_wheel), which is
	// separate from the poll counts the game reads via IbisInputEngine.MouseWheel_*.
	// Previously the parts path read the shared poll counts, but the game's per-frame
	// MouseWheel_ClearCount usually ran first and wiped the notch before the hovered
	// part ever saw it (so the wheel opened the log with a real mouse but not with the
	// deterministic test input, purely by call-order luck). With a dedicated
	// accumulator that we clear ourselves, the hovered part reliably gets every notch
	// and the game's poll still gets its own copy — neither steals from the other.
	if (parts_began_click) {
		int wheel_fwd = 0, wheel_back = 0;
		mouse_get_parts_wheel(&wheel_fwd, &wheel_back);
		if (wheel_fwd || wheel_back) {
			// "Whole" (global) wheel handlers register on parts_no 0
			// (AddWholeMouseWheelEvent). The backlog/save/load/CG/replay UIs
			// scroll this way: CBackLogView@MouseWheelEvent is a whole handler,
			// reached only by a parts_no==0 message -> scrollbar pos ->
			// ScrollEvent -> SetLineIndex. The hovered text parts have no
			// per-part delegate, so without this the notch was discarded and the
			// view never rebuilt (dead scroll). Post once per notch, regardless
			// of hover; consumed harmlessly if no whole handler is registered.
			parts_msg_push_global(PARTS_MSG_MOUSE_WHEEL, "ii", wheel_fwd, wheel_back);
			bool delivered = false;
			PARTS_LIST_FOREACH_REVERSE(parts) {
				if (parts->is_hovered && parts->wheelable) {
					parts_msg_push(parts, PARTS_MSG_MOUSE_WHEEL, "ii",
							wheel_fwd, wheel_back);
					delivered = true;
					if (getenv("XSYS4_BL_TRACE"))
						NOTICE("WHEEL deliver part=%d fwd=%d back=%d", parts->no, wheel_fwd, wheel_back);
				}
			}
			if (getenv("XSYS4_BL_TRACE") && !delivered)
				NOTICE("WHEEL fwd=%d back=%d but NO hovered part (pos=%d,%d began=%d)",
				       wheel_fwd, wheel_back, cur_pos.x, cur_pos.y, parts_began_click);
			mouse_clear_parts_wheel();
		}
	}

	// Horizontal scrollbar (config sliders): grab on press anywhere in the track
	// band, then follow the cursor. The knob CG is small, so hit-test the whole
	// track rectangle for a forgiving grab + click-to-jump.
	if (cur_clicking && !prev_clicking && !dragging_scrollbar) {
		PARTS_LIST_FOREACH_REVERSE(parts) {
			if (!parts->global.show)
				continue;
			if (!parts->is_hscrollbar && !parts->is_vscrollbar)
				continue;
			int px = parts->parent ? parts->parent->global.pos.x : 0;
			int py = parts->parent ? parts->parent->global.pos.y : 0;
			// h-bar track: length(x)×width(y); v-bar track: width(x)×length(y).
			Rectangle track = parts->is_hscrollbar
				? (Rectangle){ px + parts->sb_base_x, py + parts->sb_base_y,
						parts->sb_length, parts->sb_width }
				: (Rectangle){ px + parts->sb_base_x, py + parts->sb_base_y,
						parts->sb_width, parts->sb_length };
			if (getenv("XSYS4_SLIDER_TRACE"))
				NOTICE("SLIDER track no=%d rect=%d,%d %dx%d parent=%d ppos=%d,%d "
				       "gpos=%d,%d курсор=%d,%d %s",
				       parts->no, track.x, track.y, track.w, track.h,
				       parts->parent ? parts->parent->no : -1, px, py,
				       parts->global.pos.x, parts->global.pos.y,
				       cur_pos.x, cur_pos.y,
				       SDL_PointInRect(&cur_pos, &track) ? "ПОПАЛ" : "");
			if (SDL_PointInRect(&cur_pos, &track)) {
				dragging_scrollbar = parts;
				break;
			}
		}
	}
	if (dragging_scrollbar) {
		if (cur_clicking) {
			if (dragging_scrollbar->is_hscrollbar) {
				// ★Протяжку НАДО ОБЪЯВИТЬ ИГРЕ. Долю она не опрашивает: замер
				// XSYS4_SLIDER_TRACE на вкладке `メッセージウィンドウ` дал 11
				// протяжек и НОЛЬ вызовов GetHSliderBarScrollRate. Ползунок
				// ехал, а прозрачность окна не менялась.
				// Игра ждёт событие прокрутки: `CAlpha@Active::postset` вешает
				// на часть DG_ScrollHandler(number, scrollPos, total), и уже
				// обработчик читает долю и зовёт SetMessageWindowAlphaRate.
				// У вертикальной полосы это делалось (строкой ниже), у
				// горизонтальной — нет; отсюда и «ползунок не меняет превью».
				float old_rate = dragging_scrollbar->hscroll_rate;
				parts_hscrollbar_drag_to(dragging_scrollbar, cur_pos.x);
				if (dragging_scrollbar->hscroll_rate != old_rate)
					PE_OnHScrollbarDragged(dragging_scrollbar->no,
							dragging_scrollbar->hscroll_rate);
			} else {
				parts_vscrollbar_drag_to(dragging_scrollbar, cur_pos.y);
				PE_OnVScrollbarDragged(dragging_scrollbar->no,
						dragging_scrollbar->vscroll_rate);
			}
		} else {
			dragging_scrollbar = NULL;
		}
	}

	// Drag movement processing
	if (drag_parts && cur_clicking) {
		bool cursor_moved = (cur_pos.x != parts_prev_pos.x ||
				cur_pos.y != parts_prev_pos.y);
		if (cursor_moved && drag_parts->draggable) {
			Point new_pos = {
				drag_initial_pos.x + (cur_pos.x - drag_start_cursor.x),
				drag_initial_pos.y + (cur_pos.y - drag_start_cursor.y)
			};
			parts_set_pos(drag_parts, new_pos);
			if (!is_dragging) {
				parts_msg_push(drag_parts, PARTS_MSG_DRAG_BEGIN, "");
			}
			parts_msg_push(drag_parts, PARTS_MSG_DRAGGING, "iiii",
					drag_start_cursor.x, drag_start_cursor.y, cur_pos.x, cur_pos.y);
			is_dragging = true;
		}

		// Drop target tracking
		if (cursor_moved && is_dragging) {
			struct parts *new_drop = NULL;
			PARTS_LIST_FOREACH_REVERSE(parts) {
				if (parts == drag_parts)
					continue;
				if (parts_hittest(parts, PARTS_STATE_DEFAULT, cur_pos)) {
					new_drop = parts;
					if (!parts->pass_cursor)
						break;
				}
			}
			if (new_drop != drop_target) {
				if (drop_target) {
					parts_msg_push(drop_target, PARTS_MSG_DROP_LEAVE,
							"i", drag_parts->no);
				}
				if (new_drop) {
					parts_msg_push(new_drop, PARTS_MSG_DROP_ENTER,
							"i", drag_parts->no);
				}
				drop_target = new_drop;
			} else if (drop_target) {
				parts_msg_push(drop_target, PARTS_MSG_DROP_ON,
						"iii", drag_parts->no, cur_pos.x, cur_pos.y);
			}
		}
	}

	// «Whole» (глобальные) обработчики левого клика регистрируются на parts_no 0
	// (AddWholeMouseLClickEvent), и CallDelegate отдаёт сообщение с parts_no == 0
	// в m_wholeFunctionSet, минуя проверки delegateIndex/uniqueID. Через них игра
	// принимает КЛИКИ СЦЕН: SceneStack@RegisterEvent вешает туда свой <LClickEvent>,
	// а тот уже зовёт LClickEvent активной сцены. Пер-партовое MOUSE_CLICK этот путь
	// не покрывает — оно уходит в набор функций конкретной части, где обработчика нет.
	// Без этого сообщения Haha Ranman навсегда вставала на экране 注意: CautionScene
	// доходила до Process == 1 («жду клик»), клик доезжал до части-фона CG, а сцена
	// его не видела — main не добирался ни до сети, ни до титула.
	// Шлём на ОТПУСКАНИЕ кнопки и БЕЗУСЛОВНО (даже если клик съела часть): whole-набор
	// по смыслу «клик куда угодно», и его m_partsNumber всё равно 0. Если whole-обработчика
	// нет, диспетчер съедает сообщение безвредно.
	if (parts_began_click && prev_clicking && !cur_clicking) {
		parts_msg_push_global(PARTS_MSG_MOUSE_CLICK,
				"iii", cur_pos.x, cur_pos.y, VK_LBUTTON);
	}

	// Release handling
	if (prev_clicking && !cur_clicking) {
		if (is_dragging && drag_parts && drag_parts->draggable) {
			parts_msg_push(drag_parts, PARTS_MSG_DRAG_END, "");
		}
		if (drop_target && drag_parts) {
			parts_msg_push(drop_target, PARTS_MSG_DROPPED,
					"iii", drag_parts->no, cur_pos.x, cur_pos.y);
			parts_msg_push(drop_target, PARTS_MSG_DROP_LEAVE, "i", drag_parts->no);
		}
		drag_state_reset();

		if (!click_down_parts) {
			// TODO: play misclick sound
		}
		click_down_parts = 0;
	}

	prev_clicking = cur_clicking;
	parts_prev_pos = cur_pos;
}

/*
 * «Ввод подтверждён» текстовому полю. Живёт здесь, а не в PartsEngine.c: очередь
 * сообщений частей — внутренняя кухня parts, наружу торчит только этот вызов.
 */
void PE_SendFixedEvent(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return;
	parts_msg_push(parts, PARTS_MSG_FIXED, "");
}

void PE_SetPassCursor(int parts_no, bool pass)
{
	parts_get(parts_no)->pass_cursor = !!pass;
}

// `マウスカーソルピクセル判定` из раскладки (см. parts->pixel_hittest). В библиотеке
// PartsEngine функции для этого нет — поле читает загрузчик раскладок act_build_part.
void PE_SetPartsPixelHitTest(int parts_no, bool enable)
{
	parts_get(parts_no)->pixel_hittest = !!enable;
}

bool PE_GetPartsPassCursor(int parts_no)
{
	return parts_get(parts_no)->pass_cursor;
}

void PE_SetClickable(int parts_no, bool clickable)
{
	// Ноль тут не годится: он означает «игра молчала» (см. поле в parts_internal.h).
	parts_get(parts_no)->clickable = clickable ? 1 : -1;
}

void PE_SetPartsIsButton(int parts_no, bool is_button)
{
	parts_get(parts_no)->is_button = !!is_button;
}

void PE_SetEnableInputProcess(int parts_no, bool enable)
{
	if (getenv("XSYS4_EIP_TRACE")) {
		NOTICE("EIP SetEnableInputProcess(%d, %d)", parts_no, (int)enable);
		if (getenv("XSYS4_EIP_TRACE_STACK"))
			vm_stack_trace();
	}
	parts_get(parts_no)->enable_input_process = !!enable;
}

bool PE_IsEnableInputProcess(int parts_no)
{
	return parts_get(parts_no)->enable_input_process;
}

void PE_SetPartsWheelable(int parts_no, bool wheelable)
{
	parts_get(parts_no)->wheelable = !!wheelable;
}

bool PE_GetPartsClickable(int parts_no)
{
	// Игре видны только «да/нет»: −1 (запрещено явно) и 0 (не задано) — оба «нет».
	return parts_get(parts_no)->clickable > 0;
}

void PE_SetPartsGroupDecideOnCursor(possibly_unused int group_no, possibly_unused bool decide_on_cursor)
{
	UNIMPLEMENTED("(%d, %s)", group_no, decide_on_cursor ? "true" : "false");
}

void PE_SetPartsGroupDecideClick(possibly_unused int group_no, possibly_unused bool decide_click)
{
	UNIMPLEMENTED("(%d, %s)", group_no, decide_click ? "true" : "false");
}

void PE_SetOnCursorShowLinkPartsNumber(int parts_no, int link_parts_no)
{
	struct parts *parts = parts_get(parts_no);
	struct parts *link_parts = parts_get(link_parts_no);
	parts->linked_to = link_parts_no;
	link_parts->linked_from = parts_no;
}

int PE_GetOnCursorShowLinkPartsNumber(int parts_no)
{
	return parts_get(parts_no)->linked_to;
}

bool PE_SetPartsOnCursorSoundNumber(int parts_no, int sound_no)
{
	if (!asset_exists(ASSET_SOUND, sound_no)) {
		WARNING("Invalid sound number: %d", sound_no);
		return false;
	}

	struct parts *parts = parts_get(parts_no);
	parts->on_cursor_sound = sound_no;
	return true;
}

bool PE_SetPartsClickSoundNumber(int parts_no, int sound_no)
{
	if (!asset_exists(ASSET_SOUND, sound_no)) {
		WARNING("Invalid sound number: %d", sound_no);
		return false;
	}

	struct parts *parts = parts_get(parts_no);
	parts->on_click_sound = sound_no;
	return true;
}

// Parts_SetSoundNumber/Parts_GetSoundNumber — звук части ПО СОСТОЯНИЮ.
//
// Состояние здесь 1-based, как у всего остального parts-API (ср. PE_SetPartsCG с `--state`):
// 1 — обычное, 2 — под курсором, 3 — клик. Мэппинг взят НЕ из догадки, а из байткода игры:
// у Escalayer `AFL_Parts_GetOnCursorSound` зовёт Parts_GetSoundNumber с 2, а
// `AFL_Parts_GetClickSound` — с 3. У обычного состояния своего звука нет.
//
// ★Граница «нет звука»: игра считает таковым НОЛЬ (`AFL_Parts_PlaySound` выходит сразу,
// если SoundNumber == 0), а у нас это −1 (parts_play_sound играет только >= 0). Поэтому
// переводим на границе в обе стороны — иначе «пустой» звук либо потерялся бы, либо мы
// попытались бы играть несуществующий номер 0 (ровно тот шум, который убран в §5ah).
void PE_Parts_SetSoundNumber(int parts_no, int sound_no, int state)
{
	struct parts *parts = parts_get(parts_no);
	if (sound_no <= 0)
		sound_no = -1;
	switch (state) {
	case 2: parts->on_cursor_sound = sound_no; break;
	case 3: parts->on_click_sound = sound_no; break;
	default: break;
	}
}

int PE_Parts_GetSoundNumber(int parts_no, int state)
{
	struct parts *parts = parts_get(parts_no);
	int sound_no;
	switch (state) {
	case 2: sound_no = parts->on_cursor_sound; break;
	case 3: sound_no = parts->on_click_sound; break;
	default: return 0;
	}
	return sound_no >= 0 ? sound_no : 0;
}

bool PE_SetClickMissSoundNumber(possibly_unused int sound_no)
{
	UNIMPLEMENTED("(%d)", sound_no);
	return true;
}

void PE_BeginInput(void)
{
	parts_input_depth++;
	parts_began_click = true;
	if (getenv("XSYS4_CLICKNO_TRACE"))
		NOTICE("CLICKNO BeginInput -> глубина %d", parts_input_depth);
}

void PE_EndInput(void)
{
	if (parts_input_depth > 0)
		parts_input_depth--;
	parts_began_click = parts_input_depth > 0;
	if (getenv("XSYS4_CLICKNO_TRACE"))
		NOTICE("CLICKNO EndInput -> глубина %d, номер %d обнулён",
		       parts_input_depth, clicked_parts);
	clicked_parts = 0;
	drag_state_reset();
}

// Returning to the title (system.Reset) abandons any open input session; without
// this the depth would leak and input could never be disabled again.
void parts_input_reset(void)
{
	parts_input_depth = 0;
	parts_began_click = false;
	clicked_parts = 0;
	click_down_parts = 0;
	drag_state_reset();
}

void PE_SetDrag(int parts_no, bool enable)
{
	parts_get(parts_no)->draggable = !!enable;
}

/*
 * `スワイプ` — вид инерционного перетаскивания. Геттер у пары есть
 * (Parts_GetSwipeType), то есть no-op отличим: значение обязано хранИться
 * и возвращаться. Само поведение свайпа не реализовано — см. комментарий у
 * поля swipe_type. Без этой пары `ScrollBase@0` (список предметов) падал на
 * «Unimplemented HLL function: PartsEngine.Parts_SetSwipeType».
 */
void PE_SetSwipeType(int parts_no, int type)
{
	parts_get(parts_no)->swipe_type = type;
}

int PE_GetSwipeType(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->swipe_type : 0;
}

int PE_GetClickPartsNumber(void)
{
	return clicked_parts;
}

bool PE_IsCursorIn(int parts_no, int mouse_x, int mouse_y, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return false;

	Point mouse_pos = { mouse_x, mouse_y };
	return parts_hittest(parts, state, mouse_pos);
}
