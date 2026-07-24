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
	int type;
	int variables[PARTS_MSG_MAX_VARS];
	int nr_variables;
};

static STAILQ_HEAD(, parts_message) msg_queue =
		STAILQ_HEAD_INITIALIZER(msg_queue);

static void msg_push_v(int parts_no, int delegate_index, int type,
		const char *fmt, va_list ap)
{
	struct parts_message *msg = xmalloc(sizeof(*msg));
	msg->parts_no = parts_no;
	msg->delegate_index = delegate_index;
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
	msg_push_v(parts->no, parts->delegate_index, type, fmt, ap);
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
	msg_push_v(0, -1, type, fmt, ap);
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
	STAILQ_REMOVE_HEAD(&msg_queue, entry);
	free(msg);
}

int PE_GetMessageType(void)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	return msg ? msg->type : 0;
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
	if (p && p->delegate_index >= 0)
		return p->delegate_index;
	return msg->delegate_index;
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
