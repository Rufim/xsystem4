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

#include "asset_manager.h"
#include "audio.h"
#include "input.h"
#include "xsystem4.h"
#include "parts_internal.h"

// true between calls to BeginClick and EndClick
bool parts_began_click = false;

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
	return SDL_PointInRect(&pos, &hitbox);
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

static void parts_update_mouse(struct parts *parts, Point cur_pos, bool cur_clicking,
		int passed_time, bool *hover_consumed, bool *click_consumed)
{
	// Always use DEFAULT state hitbox regardless of the current display state.
	bool is_hovered = parts_hittest(parts, PARTS_STATE_DEFAULT, cur_pos)
		&& !*hover_consumed;

	bool was_hovered = parts->is_hovered;
	parts->is_hovered = is_hovered;

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

	bool click_eligible = parts->clickable || !parts->pass_cursor;
	if (!click_eligible || *click_consumed) {
		if (!was_hovered)
			audio_play_sound(parts->on_cursor_sound);
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

		// KEY_TRIGGER message fires on press transition
		parts_msg_push(parts, PARTS_MSG_KEY_TRIGGER, "i", VK_LBUTTON);
	}

	// KEY_DOWN message fires every frame while held (not first frame)
	if (prev_clicking && cur_clicking && click_down_parts == parts->no) {
		parts_msg_push(parts, PARTS_MSG_KEY_DOWN, "i", VK_LBUTTON);
	}

	if (cur_clicking && click_down_parts == parts->no) {
		parts_set_state(parts, PARTS_STATE_CLICKED);
	} else {
		if (!was_hovered) {
			audio_play_sound(parts->on_cursor_sound);
		}
		parts_set_state(parts, PARTS_STATE_HOVERED);
	}

	// click event: only if the click down event had same parts number
	if (parts->clickable && prev_clicking && !cur_clicking
			&& click_down_parts == parts->no) {
		audio_play_sound(parts->on_click_sound);
		clicked_parts = parts->no;

		// Checkbox: flip state on click; the game reads it via IsCheckBoxChecked.
		if (parts->is_checkbox)
			parts_checkbox_toggle(parts);

		parts_msg_push(parts, PARTS_MSG_MOUSE_CLICK,
				"iii", cur_pos.x, cur_pos.y, VK_LBUTTON);
		parts_msg_push(parts, PARTS_MSG_KEY_UP, "i", VK_LBUTTON);
	}
}

void PE_UpdateInputState(int passed_time)
{
	Point cur_pos;
	bool cur_clicking = key_is_down(VK_LBUTTON);
	mouse_get_pos(&cur_pos.x, &cur_pos.y);

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

	bool hover_consumed = false;
	bool click_consumed = false;
	struct parts *parts;
	// Iterate front-to-back (highest z first) for proper cursor consumption
	PARTS_LIST_FOREACH_REVERSE(parts) {
		parts_update_mouse(parts, cur_pos, cur_clicking, passed_time,
				&hover_consumed, &click_consumed);
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
				if (parts->is_hovered) {
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
			if (!parts->is_hscrollbar || !parts->global.show)
				continue;
			int px = parts->parent ? parts->parent->global.pos.x : 0;
			int py = parts->parent ? parts->parent->global.pos.y : 0;
			Rectangle track = { px + parts->sb_base_x, py + parts->sb_base_y,
					parts->sb_length, parts->sb_width };
			if (SDL_PointInRect(&cur_pos, &track)) {
				dragging_scrollbar = parts;
				break;
			}
		}
	}
	if (dragging_scrollbar) {
		if (cur_clicking)
			parts_hscrollbar_drag_to(dragging_scrollbar, cur_pos.x);
		else
			dragging_scrollbar = NULL;
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

void PE_SetPassCursor(int parts_no, bool pass)
{
	parts_get(parts_no)->pass_cursor = !!pass;
}

bool PE_GetPartsPassCursor(int parts_no)
{
	return parts_get(parts_no)->pass_cursor;
}

void PE_SetClickable(int parts_no, bool clickable)
{
	parts_get(parts_no)->clickable = !!clickable;
}

void PE_SetPartsIsButton(int parts_no, bool is_button)
{
	parts_get(parts_no)->is_button = !!is_button;
}

bool PE_GetPartsClickable(int parts_no)
{
	return parts_get(parts_no)->clickable;
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

bool PE_SetClickMissSoundNumber(possibly_unused int sound_no)
{
	UNIMPLEMENTED("(%d)", sound_no);
	return true;
}

void PE_BeginInput(void)
{
	parts_began_click = true;
}

void PE_EndInput(void)
{
	parts_began_click = false;
	clicked_parts = 0;
	drag_state_reset();
}

void PE_SetDrag(int parts_no, bool enable)
{
	parts_get(parts_no)->draggable = !!enable;
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
