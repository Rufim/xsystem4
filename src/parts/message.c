/* Copyright (C) 2026 kichikuou <KichikuouChrome@gmail.com>
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

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "system4.h"
#include "system4/string.h"

#include "vm.h"
#include "parts.h"
#include "parts_internal.h"

#define PARTS_MSG_MAX_VARS 4

struct parts_message {
	STAILQ_ENTRY(parts_message) entry;
	int parts_no;
	int delegate_index;
	int unique_id;
	int type;
	int variables[PARTS_MSG_MAX_VARS];
	int nr_variables;
};

static STAILQ_HEAD(, parts_message) msg_queue =
		STAILQ_HEAD_INITIALIZER(msg_queue);

// Значение GetMessageType на ПУСТОЙ очереди — «сообщений больше нет». Игра крутит
// drain-луп именно по этому сентинелу, и он СМЕНИЛСЯ между версиями библиотеки:
//   Tsumamigui 3 (v7) parts::detail::CPartsMessageManager@Update @0x155d36:
//     type = GetMessageType(); while (type != 0) { CallDelegate(type); PopMessage(); ... }
//   Dohna (v14)        parts::detail::CPartsMessageManager@Update @0x2c7924:
//     type = GetMessageType(); while (type != -1) { CallDelegate(type); PopMessage(); ... }
// С прежним жёстким 0 у Dohna луп не заканчивался НИКОГДА: движок отдавал 0, игра
// считала это настоящим сообщением типа 0 и вечно скармливала его CallEvent2<int,int>
// (~1 млн вызовов/с). Луп сидит внутри View_Update, поэтому кадр не завершался —
// за 20 с прогона отрисовывался РОВНО ОДИН кадр, таймеры/задачи не тикали, и сцена
// логотипа не могла уйти дальше (SceneLogo@Run ждёт DelayedCallback→ShowLogo).
// Гейт структурный и привязан к самой этой фиче: SeekMessage/GetMessageUniqueID
// объявлены ТОЛЬКО в новом message-API (тул ainliball: у Dohna есть, у Tsumamigui 3
// и Escalayer Reboot нет ни одной из двух).
static int msg_empty_type = 0;

void PE_set_message_empty_type_minus_one(void)
{
	msg_empty_type = -1;
}

// ★НОМЕРА ТИПОВ СООБЩЕНИЙ У IXSEAL ДРУГИЕ. Внутри движок нумерует по v7
// (enum parts_message_type), а игра разбирает тип своим SWITCH в
// `CPartsMessageManager@CallDelegate` — и таблицы разъехались. Обе сняты с самих игр
// (case → CallFunction*/CallDelegate* по switch-таблице): у Dohna @0x2c6eb6 SWITCH 289
// (30 ветвей), у Tsumamigui 3 @0x155dae SWITCH 50 (28 ветвей).
//   имя            v7  v14
//   MouseEnter      1   0     KeyTrigger     14  15
//   MouseMove       2   1     KeyDown        15  16
//   MouseLeave      3   2     KeyPress       16  17
//   MouseWheel      4   3     KeyUp          17  18
//   MouseClick      5   4     Focus          18  19
//   MouseDoubleClick —   5    LostFocus      19  20
//   MouseOnCursor   6   6     Scroll         20  21
//   DragBegin       7   7
//   Draging         8   8
//   DragEnd         9  10
//   DropEnter..DropLeave 10..13 → 11..14
// Сдвиг НЕ равномерный: у v14 нумерация с нуля, между MouseClick и MouseOnCursor
// ВСТАВЛЕН MouseDoubleClick (у v7 его нет вовсе), а номер 9 у v14 не занят ничем.
// Из-за этого клик (внутренний 5) приходил игре как MouseDoubleClick — она честно
// диспатчила двойной клик, на который никто не подписан, и НИ ОДИН пункт меню титула
// Dohna не работал, хотя сообщение доходило и оба гейта CallDelegate проходило.
static bool msg_ixseal_types = false;

void PE_set_message_types_ixseal(void)
{
	msg_ixseal_types = true;
}

// «Новый message-API» (тот же признак, что и выше: объявлена PartsEngine.SeekMessage).
// Нужен слою ввода: только в нём `delegate_index` — достоверный признак того, что часть
// вообще способна получить сообщение (см. parts_can_take_cursor в parts/input.c).
bool parts_msg_api_new(void)
{
	return msg_ixseal_types;
}

static int msg_type_out(int type)
{
	if (!msg_ixseal_types)
		return type;
	switch (type) {
	case PARTS_MSG_MOUSE_ENTER: return 0;
	case PARTS_MSG_MOUSE_MOVE:  return 1;
	case PARTS_MSG_MOUSE_LEAVE: return 2;
	case PARTS_MSG_MOUSE_WHEEL: return 3;
	case PARTS_MSG_MOUSE_CLICK: return 4;
	case PARTS_MSG_MOUSE_ON:    return 6;
	case PARTS_MSG_DRAG_BEGIN:  return 7;
	case PARTS_MSG_DRAGGING:    return 8;
	case PARTS_MSG_DRAG_END:    return 10;
	case PARTS_MSG_DROP_ENTER:  return 11;
	case PARTS_MSG_DROP_ON:     return 12;
	case PARTS_MSG_DROPPED:     return 13;
	case PARTS_MSG_DROP_LEAVE:  return 14;
	case PARTS_MSG_KEY_TRIGGER: return 15;
	case PARTS_MSG_KEY_DOWN:    return 16;
	case PARTS_MSG_KEY_PRESS:   return 17;
	case PARTS_MSG_KEY_UP:      return 18;
	case PARTS_MSG_SCROLL:      return 21;
	/*
	 * `Fixed` («ввод подтверждён») в нумерации v14 — 26, а не 25: движок слал номер,
	 * снятый с ДРУГОЙ игры, и предупреждение «type 25 has no Ixseal number» оставалось
	 * незамеченным, потому что 25 у v14 тоже занят (ChangedFlg) и сообщение молча
	 * уходило не туда. Из-за этого пропадала заметка к сейву: игра подписывалась
	 * (`CTextBoxParts@FixedEvent::add` — 2 вызова), сообщение доходило и вычитывалось
	 * (`MSG POP type=25 parts=90000443 nvars=0`), а обработчик не вызывался ни разу.
	 *
	 * ★Номера сняты с диспетчера ЭТОЙ игры (`CPartsMessageManager@CallDelegate`,
	 * SWITCH 279): 30 веток идут в порядке кода — MouseEnter, MouseMove, MouseLeave,
	 * MouseWheel, MouseClick, **MouseDoubleClick**, MouseOnCursor, DragBegin, Draging,
	 * DragEnd, DropEnter, DropOn, Dropped, DropLeave, KeyTrigger, KeyDown, KeyPress,
	 * KeyUp, Focus, LostFocus, Scroll, Changed, ChangedFlg, Created, Selected, **Fixed**,
	 * SwipeBegin, Swiping, SwipeEnd, Closing, — и значения `.CASE` в том же порядке дают
	 * 0,1,2,3,4,5,6,7,8,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30.
	 * Все восемнадцать номеров ВЫШЕ этой таблицы совпадают с такой раскладкой один в
	 * один — это и есть её проверка.
	 */
	case PARTS_MSG_FIXED:       return 26;
	}
	// Вместо тихого дефолта — проверка допущения: enum выше покрыт целиком, поэтому
	// сюда попадает только НОВЫЙ тип, для которого номер v14 ещё не установлен.
	static bool warned = false;
	if (!warned) {
		warned = true;
		WARNING("parts message type %d has no Ixseal (v14) number — passing through",
			type);
	}
	return type;
}

static void msg_push_v(int parts_no, int delegate_index, int unique_id, int type,
		const char *fmt, va_list ap)
{
	struct parts_message *msg = xmalloc(sizeof(*msg));
	msg->parts_no = parts_no;
	msg->delegate_index = delegate_index;
	msg->unique_id = unique_id;
	msg->type = type;
	msg->nr_variables = 0;

	for (const char *p = fmt; *p; p++) {
		if (msg->nr_variables >= PARTS_MSG_MAX_VARS) {
			VM_ERROR("parts_msg_push: too many variables");
		}
		switch (*p) {
		case 'i':
			msg->variables[msg->nr_variables++] = va_arg(ap, int);
			break;
		default:
			VM_ERROR("parts_msg_push: unsupported format '%c'", *p);
		}
	}

	STAILQ_INSERT_TAIL(&msg_queue, msg, entry);
}

void parts_msg_push(struct parts* parts, int type, const char *fmt, ...)
{
	// Message system is introduced in Rance 9
	if (!parts_multi_controller) {
		if (getenv("XSYS4_MSG_TRACE"))
			NOTICE("MSG DROP (no multi_controller) type=%d parts=%d", type, parts->no);
		return;
	}
	if (getenv("XSYS4_MSG_TRACE") && type != 6 /*skip per-frame MOUSE_ON spam*/)
		NOTICE("MSG PUSH type=%d parts=%d delegate=%d", type, parts->no, parts->delegate_index);

	va_list ap;
	va_start(ap, fmt);
	msg_push_v(parts->no, parts->delegate_index, parts->event_unique_id, type, fmt, ap);
	va_end(ap);
}

// Push a "whole" (global) message with parts_no == 0. The game's
// CPartsMessageManager dispatches a parts_no==0 message to its m_WholeFunctionSet
// (handlers registered via parts::AddWhole*Event, which key on parts_no 0), rather
// than to a per-part delegate. Needed for whole mouse-wheel handlers: e.g.
// backlog/save/load/CG/replay register CBackLogView@MouseWheelEvent etc. via
// AddWholeMouseWheelEvent, so the notch only reaches them through a parts_no==0
// message. If no whole handler is registered, the dispatcher consumes it harmlessly.
void parts_msg_push_global(int type, const char *fmt, ...)
{
	if (!parts_multi_controller) {
		if (getenv("XSYS4_MSG_TRACE"))
			NOTICE("MSG DROP (no multi_controller) type=%d parts=0 (whole)", type);
		return;
	}
	if (getenv("XSYS4_MSG_TRACE") && type != 6)
		NOTICE("MSG PUSH type=%d parts=0 (whole)", type);

	va_list ap;
	va_start(ap, fmt);
	msg_push_v(0, -1, -1, type, fmt, ap);
	va_end(ap);
}

void PE_ReleaseMessage(void)
{
	while (!STAILQ_EMPTY(&msg_queue)) {
		struct parts_message *msg = STAILQ_FIRST(&msg_queue);
		STAILQ_REMOVE_HEAD(&msg_queue, entry);
		free(msg);
	}
}

void PE_PopMessage(void)
{
	if (STAILQ_EMPTY(&msg_queue))
		return;
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	if (getenv("XSYS4_MSG_TRACE") && msg->type != 6)
		NOTICE("MSG POP  type=%d parts=%d delegate=%d nvars=%d",
		       msg->type, msg->parts_no, msg->delegate_index, msg->nr_variables);
	// XSYS4_MSG_STACK=<тип>: одноразовый стек VM на снятии сообщения этого типа. За
	// один прогон даёт ВСЮ игровую цепочку обработки — именно так найден диспетчер
	// Ixseal (SceneTitle@Run → SceneContext@Join → Ｐ＿クリック実行 → WaitForClick →
	// AFL_View_Update → UpdateMessage → CPartsMessageManager@Update).
	{
		static bool traced = false;
		const char *want = getenv("XSYS4_MSG_STACK");
		if (want && !traced && msg->type == atoi(want)) {
			traced = true;
			NOTICE("MSG STACK for type=%d parts=%d:", msg->type, msg->parts_no);
			vm_stack_trace();
		}
	}
	STAILQ_REMOVE_HEAD(&msg_queue, entry);
	free(msg);
}

int PE_GetMessageType(void)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	return msg ? msg_type_out(msg->type) : msg_empty_type;
}

int PE_GetMessagePartsNumber(void)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	return msg ? msg->parts_no : 0;
}

int PE_GetMessageDelegateIndex(void)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	if (!msg)
		return -1;
	// Prefer the part's *current* delegate index: some messages (e.g. a Scroll
	// posted from SetVScrollbarScrollPos during InitVScrollbar) are queued before
	// the game registers the handler / assigns the delegate index, so the value
	// captured at push time can be stale (-1).
	struct parts *p = parts_try_get(msg->parts_no);
	int r = (p && p->delegate_index >= 0) ? p->delegate_index : msg->delegate_index;
	if (getenv("XSYS4_MSG_TRACE") && msg->type != 6)
		NOTICE("MSG READ delegate_index -> %d (type=%d parts=%d)", r, msg->type, msg->parts_no);
	return r;
}

// Парный к GetMessageDelegateIndex: `uniqueID` набора обработчиков, который поставил
// SetEventID (см. PE_SetEventID). `CPartsMessageManager@CallDelegate` сверяет его с
// `GetUniqueID` найденного по индексу набора и при несовпадении сообщение НЕ
// диспатчит. Актуальное значение части предпочитаем снятому на push по той же
// причине, что и у индекса делегата: сообщение могло попасть в очередь до того,
// как игра зарегистрировала обработчик.
int PE_GetMessageUniqueID(void)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	if (!msg)
		return -1;
	struct parts *p = parts_try_get(msg->parts_no);
	int r = (p && p->event_unique_id >= 0) ? p->event_unique_id : msg->unique_id;
	if (getenv("XSYS4_MSG_TRACE") && msg->type != 6)
		NOTICE("MSG READ unique_id -> %d (type=%d parts=%d)", r, msg->type, msg->parts_no);
	return r;
}

int PE_GetMessageVariableCount(void)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	return msg ? msg->nr_variables : 0;
}

int PE_GetMessageVariableType(possibly_unused int index)
{
	// In the current implementation, all variables are integers.
	return 1;
}

int PE_GetMessageVariableInt(int index)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	if (!msg)
		return 0;
	if (index < 0 || index >= msg->nr_variables)
		return 0;
	return msg->variables[index];
}

float PE_GetMessageVariableFloat(possibly_unused int index)
{
	return 0.0f;
}

bool PE_GetMessageVariableBool(possibly_unused int index)
{
	return false;
}

void PE_GetMessageVariableString(possibly_unused int index, struct string **out)
{
	if (*out)
		free_string(*out);
	*out = string_ref(&EMPTY_STRING);
}
