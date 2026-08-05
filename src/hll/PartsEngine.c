/* Copyright (C) 2023 Nunuhara Cabbage <nunuhara@haniwa.technology>
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

#include <stdlib.h>
#include <string.h>

#include "system4/ain.h"
#include "system4/hashtable.h"
#include "system4/string.h"
#include "system4/ex.h"
#include "system4/utfsjis.h"
#include "system4/archive.h"

#include "vm/heap.h"
#include "vm/page.h"
#include "parts.h"
#include "asset_manager.h"
#include "xsystem4.h"
#include "hll.h"

static void PartsEngine_ModuleInit(void)
{
	PE_Init();
}

static void PartsEngine_ModuleFini(void)
{
	PE_Reset();
}

static void PartsEngine_Update(int passed_time, bool is_skip, bool message_window_show)
{
	PE_Update(passed_time, message_window_show);
}

static void PartsEngine_Update_Pascha3PC(struct string *xxx1, struct string *xxx2, int passed_time, bool is_skip, bool message_window_show)
{
	PE_Update(passed_time, message_window_show);
}

// Oyako Rankan
static bool PartsEngine_AddDrawCutCGToPartsConstructionProcess_old(int parts_no,
		struct string *cg_name, int dx, int dy, int sx, int sy, int w, int h,
		int state)
{
	return PE_AddDrawCutCGToPartsConstructionProcess(parts_no, cg_name, dx, dy, w, h,
			sx, sy, w, h, 0, state);
}

// Oyako Rankan
static bool PartsEngine_AddCopyCutCGToPartsConstructionProcess_old(int parts_no,
		struct string *cg_name, int dx, int dy, int sx, int sy, int w, int h,
		int state)
{
	return PE_AddCopyCutCGToPartsConstructionProcess(parts_no, cg_name, dx, dy, w, h,
			sx, sy, w, h, 0, state);
}

// Rance 9: argument types changed from int to float
static void PartsEngine_SetComponentPos(int parts_no, float x, float y)
{
	PE_SetPos(parts_no, x, y);
}

// Rance 9: return type changed from int to float
static float PartsEngine_Parts_GetPartsUpperLeftPosX(int parts_no, int state)
{
	return PE_GetPartsUpperLeftPosX(parts_no, state);
}

static float PartsEngine_Parts_GetPartsUpperLeftPosY(int parts_no, int state)
{
	return PE_GetPartsUpperLeftPosY(parts_no, state);
}

// Rance 9: return type changed from int to float
static float PartsEngine_Parts_GetComponentPosX(int parts_no)
{
	return PE_GetPartsX(parts_no);
}

static float PartsEngine_GetComponentPosY(int parts_no)
{
	return PE_GetPartsY(parts_no);
}

// Rance 9: X/Y coordinate types changed from int to float
static void PartsEngine_AddComponentMotionPos(int parts_no, float begin_x, float begin_y,
		float end_x, float end_y, int begin_t, int end_t,
		struct string *curve_name)
{
	PE_AddMotionPos_curve(parts_no, begin_x, begin_y, end_x, end_y,
			begin_t, end_t, curve_name);
}

static void PartsEngine_add_construction_process(union vm_value *ints,
		union vm_value *floats, union vm_value *strings)
{
	int parts_no    = ints[0].i;
	int state       = ints[1].i;
	int command     = ints[2].i;
	int interp_type = ints[3].i;
	int sx          = ints[4].i;
	int sy          = ints[5].i;
	int sw          = ints[6].i;
	int sh          = ints[7].i;
	int dx          = ints[8].i;
	int dy          = ints[9].i;
	int dw          = ints[12].i;
	int dh          = ints[13].i;
	int r           = ints[14].i;
	int g           = ints[15].i;
	int b           = ints[16].i;
	int a           = ints[17].i;
	// int r2       = ints[18].i;
	// int g2       = ints[19].i;
	// int b2       = ints[20].i;
	int char_space  = ints[21].i;
	int line_space  = ints[22].i;
	int font_type   = ints[23].i;
	int font_size   = ints[24].i;
	int font_r      = ints[25].i;
	int font_g      = ints[26].i;
	int font_b      = ints[27].i;
	int edge_r      = ints[28].i;
	int edge_g      = ints[29].i;
	int edge_b      = ints[30].i;
	int full_size   = ints[31].i;
	float bold_weight = floats[0].f;
	float edge_weight = floats[1].f;
	struct string *text    = heap_get_string(strings[0].i);
	struct string *cg_name = heap_get_string(strings[1].i);

	if (getenv("XSYS4_BL_TRACE") && (command == 7 || command == 8 || command == 23 || command == 24))
		NOTICE("TEXTOP cmd=%d part=%d dx=%d dy=%d ftype=%d fsize=%d col=%d,%d,%d str0slot=%d str0len=%d text='%s'",
		       command, parts_no, dx, dy, font_type, font_size, font_r, font_g, font_b,
		       strings[0].i, text ? (int)text->size : -1, text ? display_sjis0(text->text) : "(null)");

	switch (command) {
	case 0:  // CASConstructionProcess::SetCreate
		PE_AddCreateToPartsConstructionProcess(parts_no, dw, dh, state);
		break;
	case 1:  // CASConstructionProcess::SetCreatePixelOnly
		PE_AddCreatePixelOnlyToPartsConstructionProcess(parts_no, dw, dh, state);
		break;
	case 2:  // CASConstructionProcess::SetCreateCG
		PE_AddCreateCGToProcess(parts_no, cg_name, state);
		break;
	case 3:  // CASConstructionProcess::SetFill
		PE_AddFillToPartsConstructionProcess(parts_no,
				dx, dy, dw, dh, r, g, b, state);
		break;
	case 4:  // CASConstructionProcess::SetFillAlphaColor
		PE_AddFillAlphaColorToPartsConstructionProcess(parts_no,
				dx, dy, dw, dh, r, g, b, a, state);
		break;
	case 5:  // CASConstructionProcess::SetFillAMap
		PE_AddFillAMapToPartsConstructionProcess(parts_no,
				dx, dy, dw, dh, a, state);
		break;
	case 6:  // CASConstructionProcess::SetFillWithAlpha
		PE_AddFillWithAlphaToPartsConstructionProcess(parts_no,
				dx, dy, dw, dh, r, g, b, a, state);
		break;
	case 7:  // CASConstructionProcess::SetDrawText
		PE_AddDrawTextToPartsConstructionProcess(parts_no,
				dx, dy, text, font_type, font_size,
				font_r, font_g, font_b, bold_weight,
				edge_r, edge_g, edge_b, edge_weight,
				char_space, line_space, state);
		break;
	case 8:  // CASConstructionProcess::SetCopyText
		PE_AddCopyTextToPartsConstructionProcess(parts_no,
				dx, dy, text, font_type, font_size,
				font_r, font_g, font_b, bold_weight,
				edge_r, edge_g, edge_b, edge_weight,
				char_space, line_space, state);
		break;
	case 9:  // CASConstructionProcess::SetFillGradationHorizon
		WARNING("AddConstructProcess: FillGradationHorizon unimplemented");
		break;
	case 10:  // CASConstructionProcess::SetDrawRect
		PE_AddDrawRectToPartsConstructionProcess(parts_no,
				dx, dy, dw, dh, r, g, b, state);
		break;
	case 11:  // CASConstructionProcess::SetCutCGBlend (equal scale)
		PE_AddDrawCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, dw, dh,
				interp_type, state);
		break;
	case 12:  // CASConstructionProcess::SetCutCGCopy (equal scale)
		PE_AddCopyCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, dw, dh,
				interp_type, state);
		break;
	case 13:  // CASConstructionProcess::SetCutCGScaleBlend
		PE_AddDrawCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, sw, sh,
				interp_type, state);
		break;
	case 14:  // CASConstructionProcess::SetCutCGScaleCopy
		PE_AddCopyCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, sw, sh,
				interp_type, state);
		break;
	case 15:  // CASConstructionProcess::SetGrayFilter
		PE_AddGrayFilterToPartsConstructionProcess(parts_no,
				dx, dy, dw, dh, full_size, state);
		break;
	case 16:  // CASConstructionProcess::SetAddFilter
		WARNING("AddConstructProcess: AddFilter unimplemented");
		break;
	case 17:  // CASConstructionProcess::SetMulFilter
		WARNING("AddConstructProcess: MulFilter unimplemented");
		break;
	case 18:  // CASConstructionProcess::SetDrawLine
		WARNING("AddConstructProcess: DrawLine unimplemented");
		break;
	case 19:  // CASConstructionProcess::SetCutCGAlphaBlend (equal scale)
		PE_AddDrawCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, dw, dh,
				interp_type, state);
		break;
	case 20:  // CASConstructionProcess::SetCutCGScaleAlphaBlend
		PE_AddDrawCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, sw, sh,
				interp_type, state);
		break;
	case 21:  // CASConstructionProcess::SetCutCGOnlyAlpha (equal scale)
		PE_AddCopyCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, dw, dh,
				interp_type, state);
		break;
	case 22:  // CASConstructionProcess::SetCutCGScaleOnlyAlpha
		PE_AddCopyCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, sw, sh,
				interp_type, state);
		break;
	case 23:  // CASConstructionProcess::SetAlphaBlendText
		// Text drawn onto a (usually FillAMap'd, alpha=0) transparent surface — the
		// backlog builds each line this way. Must WRITE the glyph alpha into the
		// destination, so use the copy-text path (RENDER_COPY: RGB=color, alpha=MAX).
		// The draw-text path (RENDER_BLENDED) leaves dst alpha untouched → invisible.
		PE_AddCopyTextToPartsConstructionProcess(parts_no,
				dx, dy, text, font_type, font_size,
				font_r, font_g, font_b, bold_weight,
				edge_r, edge_g, edge_b, edge_weight,
				char_space, line_space, state);
		break;
	case 24:  // CASConstructionProcess::SetOnlyAlphaText
		PE_AddCopyTextToPartsConstructionProcess(parts_no,
				dx, dy, text, font_type, font_size,
				font_r, font_g, font_b, bold_weight,
				edge_r, edge_g, edge_b, edge_weight,
				char_space, line_space, state);
		break;
	default:
		WARNING("AddConstructProcess: unknown command %d", command);
		break;
	}
}

// Generic dispatch function for PartsEngine operations.
// func_id selects the operation; arguments and return values are passed
// through three typed arrays (int/bool, float, string).
//
// Calling convention (from game script):
//   1. Push input values into the appropriate arrays.
//   2. Push a placeholder (0 or "") for each output slot.
//   3. Call PartsFunc; outputs are written back into the placeholder slots.
static int PartsEngine_PartsFunc(int func_id, struct page **array_int,
		struct page **array_float, struct page **array_string)
{
	int nr_ints = (array_int && *array_int) ? (*array_int)->nr_vars : 0;
	union vm_value *ints = nr_ints ? (*array_int)->values : NULL;
	int nr_floats = (array_float && *array_float) ? (*array_float)->nr_vars : 0;
	union vm_value *floats = nr_floats ? (*array_float)->values : NULL;
	int nr_strings = (array_string && *array_string) ? (*array_string)->nr_vars : 0;
	union vm_value *strings = nr_strings ? (*array_string)->values : NULL;

#define REQUIRE_INTS(n) \
	if (nr_ints != (n)) VM_ERROR("Invalid arguments for PartsFunc %d: expected %d ints, got %d", func_id, (n), nr_ints)
#define REQUIRE_FLOATS(n) \
	if (nr_floats < (n)) VM_ERROR("Invalid arguments for PartsFunc %d: expected %d floats, got %d", func_id, (n), nr_floats)
#define REQUIRE_STRINGS(n) \
	if (nr_strings < (n)) VM_ERROR("Invalid arguments for PartsFunc %d: expected %d strings, got %d", func_id, (n), nr_strings)

	switch (func_id) {
	case 0:  // void SetActiveLayer(int layer)
		REQUIRE_INTS(1);
		PE_set_active_controller(ints[0].i);
		return 1;
	case 1:  // int GetActiveLayer()
		REQUIRE_INTS(1);
		ints[0].i = PE_get_active_controller();
		return 1;
	case 2:  // int GetSystemOverlayLayer()
		REQUIRE_INTS(1);
		ints[0].i = PE_get_system_controller();
		return 1;
	case 3:  // void PauseMotion(bool pause)
		REQUIRE_INTS(1);
		PE_PauseMotion(!!ints[0].i);
		return 1;
	case 4:  // void SetWantSave(int parts_no, bool want_save)
		REQUIRE_INTS(2);
		PE_parts_set_want_save(ints[0].i, !!ints[1].i);
		return 1;
	case 6:  // bool SaveThumbnail(string filename, int reduction_factor)
		REQUIRE_INTS(2);
		REQUIRE_STRINGS(1);
		ints[1].i = PE_save_thumbnail(heap_get_string(strings[0].i), ints[0].i);
		return 1;
	case 40:  // float PARTS_GetAbsoluteX(int number)
		REQUIRE_INTS(1);
		REQUIRE_FLOATS(1);
		floats[0].f = PE_parts_get_absolute_x(ints[0].i);
		return 1;
	case 41:  // float PARTS_GetAbsoluteY(int number)
		REQUIRE_INTS(1);
		REQUIRE_FLOATS(1);
		floats[0].f = PE_parts_get_absolute_y(ints[0].i);
		return 1;
	case 42:  // int PARTS_GetAbsoluteZ(int number)
		REQUIRE_INTS(2);
		ints[1].i = PE_parts_get_absolute_z(ints[0].i);
		return 1;
	case 45:  // void PARTS_SetLockInputState(int number, bool lock)
		REQUIRE_INTS(2);
		PE_parts_set_lock_input_state(ints[0].i, !!ints[1].i);
		return 1;
	case 57:  // void AppendChild(int number, int child_number)
		REQUIRE_INTS(2);
		PE_SetParentPartsNumber(ints[1].i, ints[0].i);
		return 1;
	case 91:  // void SetLayoutBoxPadding(int parts_no, int top, int bottom, int left, int right)
		REQUIRE_INTS(5);
		PE_set_layoutbox_padding(ints[0].i, ints[1].i, ints[2].i, ints[3].i, ints[4].i);
		return 1;
	case 92:  // int GetLayoutBoxPaddingTop(int parts_no)
		REQUIRE_INTS(2);
		ints[1].i = PE_get_layoutbox_padding_top(ints[0].i);
		return 1;
	case 93:  // int GetLayoutBoxPaddingBottom(int parts_no)
		REQUIRE_INTS(2);
		ints[1].i = PE_get_layoutbox_padding_bottom(ints[0].i);
		return 1;
	case 94:  // int GetLayoutBoxPaddingLeft(int parts_no)
		REQUIRE_INTS(2);
		ints[1].i = PE_get_layoutbox_padding_left(ints[0].i);
		return 1;
	case 95:  // int GetLayoutBoxPaddingRight(int parts_no)
		REQUIRE_INTS(2);
		ints[1].i = PE_get_layoutbox_padding_right(ints[0].i);
		return 1;
	case 103:  // void GetPartsCGSurfaceArea(int parts_no, int *x, int *y, int *w, int *h, int state)
		REQUIRE_INTS(6);
		PE_GetPartsCGSurfaceArea(ints[0].i, &ints[1].i, &ints[2].i, &ints[3].i, &ints[4].i, ints[5].i);
		return 1;
	case 159:  // AddConstructProcess(ArrayInt[32], ArrayFloat[2], ArrayString[2])
		REQUIRE_INTS(32);
		REQUIRE_FLOATS(2);
		REQUIRE_STRINGS(2);
		PartsEngine_add_construction_process(ints, floats, strings);
		return 1;
	case 162:  // bool InitPartsMovie(int parts_no, int width, int height, int bg_r, int bg_g, int bg_b, int state)
		REQUIRE_INTS(8);
		ints[7].i = PE_init_parts_movie(ints[0].i, ints[1].i, ints[2].i, ints[3].i, ints[4].i, ints[5].i, ints[6].i);
		return 1;
	case 163:  // int GetMovieSprite(int parts_no, int state)
		REQUIRE_INTS(3);
		ints[2].i = PE_get_movie_sprite(ints[0].i, ints[1].i);
		return 1;
	default:
		WARNING("Unknown func_id: %d", func_id);
		return 0;
	}
#undef REQUIRE_INTS
}

HLL_WARN_UNIMPLEMENTED(, void, PartsEngine, StopSoundWithoutSystemSound);
// Per-part on-cursor/click sound-effect names. Audio side-effects are out of
// scope (sound is handled elsewhere / muted); accept and ignore so button setup
// (e.g. C_TITLE@EnableButton -> Ｐ＿オンカーソル効果音設定) proceeds.
static void PE_Parts_SetSoundName(int parts_no, struct string *name, int state) { (void)parts_no; (void)name; (void)state; }
static void PE_Parts_GetSoundName(int parts_no, struct string **out, int state) { (void)parts_no; (void)state; if (out) { if (*out) free_string(*out); *out = string_ref(&EMPTY_STRING); } }
// Tsumamigui 3: фон под меню сохранения. Визуальный no-op — рендер идёт своим путём.
static void PE_SetWantSaveBackScene(int parts_no, int enable)
{
	if (getenv("XSYS4_BS_TRACE"))
		NOTICE("BACKSCENE want-save part=%d enable=%d", parts_no, enable);
}
// Save-thumbnail of the parts back-scene into a save buffer. Not captured yet —
// return false (no data) so saving proceeds without a scene thumbnail.
static bool PE_SaveBackScene(struct page **buf) { (void)buf; return false; }
// Direct SaveThumbnail(filename, size) export. Newer games (Tsumamigui 3) call
// this via AutoSave when opening the town map; without it the unimplemented-HLL
// error dropped into the debugger REPL and the map never became interactive.
// Reuse PE_save_thumbnail (same as PartsFunc case 6); the int is the reduction
// factor. Returning success lets AutoSave complete and the map's input loop run.
static bool PE_SaveThumbnail(struct string *filename, int reduction_factor)
{
	return PE_save_thumbnail(filename, reduction_factor);
}
// Батч-форма построения parts (3 массива-параметра). Единый интерфейс, через
// который игра (parts::AddConstructProcess в .ain) добавляет ВСЕ операции
// построения parts-текстур: SetCreate/SetFill/SetDrawText/SetCutCG… Раскодируем
// массивы тем же путём, что и PartsFunc(159), и передаём в дешифратор.
// Раньше был no-op — из-за этого construction-parts (центр диалога 確認枠,
// прокручиваемый текст бэклога Tsumamigui 3) строились в 0×0/пустыми.
static void PE_AddPartsConstructionProcess(struct page **ai, struct page **af, struct page **as) {
	int nr_ints    = (ai && *ai) ? (*ai)->nr_vars : 0;
	int nr_floats  = (af && *af) ? (*af)->nr_vars : 0;
	int nr_strings = (as && *as) ? (*as)->nr_vars : 0;
	// Ожидаемый формат: 32 int, 2 float, 2 string (см. parts::AddConstructProcess).
	// Не падаем на неожиданном формате — просто пропускаем операцию.
	if (nr_ints < 32 || nr_floats < 2 || nr_strings < 2) {
		if (getenv("XSYS4_CP_TRACE"))
			NOTICE("AddPartsConstructionProcess: unexpected arg sizes i=%d f=%d s=%d",
			       nr_ints, nr_floats, nr_strings);
		return;
	}
	union vm_value *ints    = (*ai)->values;
	union vm_value *floats  = (*af)->values;
	union vm_value *strings = (*as)->values;
	if (getenv("XSYS4_CP_TRACE"))
		NOTICE("AddPartsConstructionProcess part=%d state=%d cmd=%d dst=(%d,%d %dx%d) rgba=%d,%d,%d,%d",
		       ints[0].i, ints[1].i, ints[2].i, ints[8].i, ints[9].i,
		       ints[12].i, ints[13].i, ints[14].i, ints[15].i, ints[16].i, ints[17].i);
	PartsEngine_add_construction_process(ints, floats, strings);
}

// Ixseal (System 4 v14) form of the batch construction interface. The classic
// call packs everything into three arrays and carries the part number and state
// in ints[0..1]; v14 passes both explicitly, adds a fourth array (ArrayPos: the
// point list of the new vector-shape commands) and grew ArrayInt from 32 to 40
// entries. Layout read from CASConstructionProcess::ArrayIntIndex::ToString:
//
//   0 Command, 1 InterpolationType, 2-5 Src{X,Y,Width,Height},
//   6-9 Dest{X,Y,X2,Y2}, 10-11 Dest{Width,Height}, 12-15 RGBA, 16-19 RGBA2,
//   20 CharSpace, 21 LineSpace, 22 FontType, 23 FontSize, 24-26 FontColorRGB,
//   27-29 EdgeColorRGB, 30 FullSize, 31 Blur, 32-33 Radius{X,Y}, 34 LineWidth,
//   35 RoundEdge, 36 RoundCorner, 37 Angle, 38 StartAngle, 39 SweepAngle
//
// The classic layout is the same sequence shifted by two, without A2 (new in
// v14) and without the nine trailing shape fields. ArrayFloat (BoldWeight,
// EdgeWeight) and ArrayString (Text, CGName) are unchanged.
//
// The command ids themselves did NOT change: 0..24 mean the same as in v7
// (verified against the Command constant each CASConstructionProcess@Set*
// method writes). v14 appended new ones (25.. : alpha-map gradations, blur and
// inverse filters, whole-CG blend and the line/polygon/circle/ellipse/arc/pie
// families), which the classic dispatcher below has no handler for.
#define IX_NR_CONSTRUCTION_INTS 40
#define NR_CLASSIC_CONSTRUCTION_COMMANDS 25

static void PE_AddPartsConstructionProcess_ix(int parts_no, struct page **ai, struct page **af,
		struct page **as, struct page **ap, int state)
{
	(void)ap;  // point list: only the v14-only vector-shape commands use it
	bool trace = !!getenv("XSYS4_CP_TRACE");
	int nr_ints    = (ai && *ai) ? (*ai)->nr_vars : 0;
	int nr_floats  = (af && *af) ? (*af)->nr_vars : 0;
	int nr_strings = (as && *as) ? (*as)->nr_vars : 0;
	if (nr_ints < IX_NR_CONSTRUCTION_INTS || nr_floats < 2 || nr_strings < 2) {
		if (trace)
			NOTICE("AddPartsConstructionProcess(ix): unexpected arg sizes i=%d f=%d s=%d",
			       nr_ints, nr_floats, nr_strings);
		return;
	}
	union vm_value *src = (*ai)->values;
	int command = src[0].i;
	if (command < 0 || command >= NR_CLASSIC_CONSTRUCTION_COMMANDS) {
		if (trace)
			NOTICE("AddPartsConstructionProcess(ix): v14-only command %d (part=%d)",
			       command, parts_no);
		return;
	}

	union vm_value ints[32] = {0};
	ints[0].i = parts_no;
	ints[1].i = state;
	ints[2].i = command;
	for (int i = 3; i <= 20; i++)   // InterpolationType .. B2
		ints[i] = src[i - 2];
	for (int i = 21; i <= 31; i++)  // CharSpace .. FullSize (A2 has no classic slot)
		ints[i] = src[i - 1];

	if (trace)
		NOTICE("AddPartsConstructionProcess(ix) part=%d state=%d cmd=%d src=(%d,%d %dx%d) dst=(%d,%d %dx%d) rgba=%d,%d,%d,%d",
		       parts_no, state, command,
		       ints[4].i, ints[5].i, ints[6].i, ints[7].i,
		       ints[8].i, ints[9].i, ints[12].i, ints[13].i,
		       ints[14].i, ints[15].i, ints[16].i, ints[17].i);
	PartsEngine_add_construction_process(ints, (*af)->values, (*as)->values);
}

// --- Подсистема Activity (именованные наборы parts) ---
// Новые игры System 4 группируют parts в именованные «активности». В движке её
// не было; минимальный реестр: имя -> список (имя_parts, номер_parts) + EX-текст.
// intent (遷移): a part with a transition method (遷移方法) closes/transitions the
// activity when clicked. destinations (遷移先) name target .pact pages (empty = just
// close). BindEndEvent registers MouseLClickEvent on parts where IsExistIntentData.
struct pe_act_part {
	struct string *name;
	int number;
	int intent_type;               // 遷移方法 (0 = no transition)
	struct string **intent_dests;  // 遷移先 (non-empty destination names)
	int nr_intent_dests;
};
struct pe_activity {
	struct pe_act_part *parts;
	int nr_parts;
	struct string *ex_text;
	int ex_id;
};
static struct hash_table *pe_activities;

static struct pe_activity *pe_act_find(struct string *name)
{
	if (!pe_activities)
		return NULL;
	return ht_get(pe_activities, name->text, NULL);
}

static void PE_Activity_free(void *p)
{
	struct pe_activity *a = p;
	for (int i = 0; i < a->nr_parts; i++) {
		free_string(a->parts[i].name);
		for (int j = 0; j < a->parts[i].nr_intent_dests; j++)
			free_string(a->parts[i].intent_dests[j]);
		free(a->parts[i].intent_dests);
	}
	free(a->parts);
	if (a->ex_text)
		free_string(a->ex_text);
	free(a);
}

static bool PE_CreateActivity(struct string *name)
{
	if (!pe_activities)
		pe_activities = ht_create(256);
	if (ht_get(pe_activities, name->text, NULL))
		return true;
	struct pe_activity *a = xcalloc(1, sizeof(*a));
	a->ex_id = -1;
	ht_put(pe_activities, name->text, NULL)->value = a;
	if (getenv("XSYS4_ACT_TRACE"))
		NOTICE("ACT CreateActivity(name='%s')", display_sjis0(name->text));
	return true;
}

static bool PE_IsExistActivity(struct string *name)
{
	return pe_act_find(name) != NULL;
}

static bool PE_ReleaseActivity(struct string *name, struct page **out)
{
	struct pe_activity *a = pe_act_find(name);
	int n = a ? a->nr_parts : 0;
	union vm_value dim = { .i = n };
	struct page *page = alloc_array(1, &dim, AIN_ARRAY_INT, 0, false);
	for (int i = 0; i < n; i++)
		page->values[i].i = a->parts[i].number;
	if (*out) {
		delete_page_vars(*out);
		free_page(*out);
	}
	*out = page;
	if (a) {
		// освободить активность и убрать из реестра (перезапись пустышкой-NULL)
		struct ht_slot *slot = ht_put(pe_activities, name->text, NULL);
		if (slot->value == a)
			slot->value = NULL;
		PE_Activity_free(a);
	}
	return true;
}

static bool PE_AddActivityParts(struct string *act, struct string *parts_name, int number)
{
	struct pe_activity *a = pe_act_find(act);
	if (!a) { PE_CreateActivity(act); a = pe_act_find(act); }
	a->parts = xrealloc_array(a->parts, a->nr_parts, a->nr_parts + 1, sizeof(*a->parts));
	a->parts[a->nr_parts].name = string_ref(parts_name);
	a->parts[a->nr_parts].number = number;
	a->nr_parts++;
	if (getenv("XSYS4_ACT_TRACE"))
		NOTICE("ACT AddActivityParts(act='%s', name='%s', number=%d)",
		       display_sjis0(act->text), display_sjis1(parts_name->text), number);
	return true;
}

/*
 * Activity layout loading (.pactex). A .pactex file is in the .ex container
 * format: a tree of parts (block "アクティビティ" -> "ルートパーツ") where each
 * node has position/visibility/alpha and type-specific info (CG name per state,
 * or button CG/flat/text), plus nested "子パーツ" children. ReadActivityFile
 * parses it and creates the real parts so the dialog/menu actually renders.
 */
static int pe_act_part_seq = 90000000;  // unique base for activity-created parts

// look up a child node by UTF-8 name (converted to the .ex file's SJIS encoding)
static struct ex_tree *act_child(struct ex_tree *t, const char *utf8)
{
	if (!t || t->is_leaf)
		return NULL;
	char *sjis = utf2sjis(utf8, strlen(utf8));
	struct ex_tree *r = ex_tree_get_child(t, sjis);
	free(sjis);
	return r;
}

static int act_int(struct ex_tree *t, const char *utf8, int dflt)
{
	struct ex_tree *c = act_child(t, utf8);
	if (c && c->is_leaf && c->leaf.value.type == EX_INT)
		return c->leaf.value.i;
	return dflt;
}

static float act_float(struct ex_tree *t, const char *utf8, float dflt)
{
	struct ex_tree *c = act_child(t, utf8);
	if (c && c->is_leaf) {
		if (c->leaf.value.type == EX_FLOAT)
			return c->leaf.value.f;
		if (c->leaf.value.type == EX_INT)
			return (float)c->leaf.value.i;
	}
	return dflt;
}

static struct string *act_str(struct ex_tree *t, const char *utf8)
{
	struct ex_tree *c = act_child(t, utf8);
	if (c && c->is_leaf && c->leaf.value.type == EX_STRING)
		return c->leaf.value.s;
	return NULL;
}

// nth element of a list-valued child, as int (floats truncated)
static int act_list_int(struct ex_tree *t, const char *utf8, int idx, int dflt)
{
	struct ex_tree *c = act_child(t, utf8);
	if (!c || !c->is_leaf || c->leaf.value.type != EX_LIST)
		return dflt;
	struct ex_list *l = c->leaf.value.list;
	if (idx < 0 || (unsigned)idx >= l->nr_items)
		return dflt;
	struct ex_value *v = &l->items[idx].value;
	if (v->type == EX_INT)
		return v->i;
	if (v->type == EX_FLOAT)
		return (int)v->f;
	return dflt;
}

// nth element of a list-valued child, as float
static float act_list_float(struct ex_tree *t, const char *utf8, int idx, float dflt)
{
	struct ex_tree *c = act_child(t, utf8);
	if (!c || !c->is_leaf || c->leaf.value.type != EX_LIST)
		return dflt;
	struct ex_list *l = c->leaf.value.list;
	if (idx < 0 || (unsigned)idx >= l->nr_items)
		return dflt;
	struct ex_value *v = &l->items[idx].value;
	if (v->type == EX_FLOAT)
		return v->f;
	if (v->type == EX_INT)
		return (float)v->i;
	return dflt;
}

static float act_float(struct ex_tree *t, const char *utf8, float dflt);

static void act_set_state_cg(int no, struct ex_tree *ti, const char *state_utf8, int state)
{
	struct ex_tree *st = act_child(ti, state_utf8);
	if (!st)
		return;
	// A state may be a Flat (パーツタイプ=20), a CG (11), or Text (13).
	struct string *flat = act_str(st, "フラット名");
	if (flat && flat->size) {
		PE_SetPartsFlat(no, flat, state);
		return;
	}
	// Text state: テキスト + テキスト装飾 (font). Used e.g. by the config sample
	// message ("サンプル") whose 通常状態 is a パーツタイプ=13 text sub-node.
	if (act_int(st, "パーツタイプ", -1) == 13) {
		struct string *txt = act_str(st, "テキスト");
		if (txt && txt->size) {
			struct ex_tree *fd = act_child(st, "テキスト装飾");
			if (fd) {
				PE_SetFont(no, act_int(fd, "フォントタイプ", 0),
					act_int(fd, "フォントサイズ", 16),
					act_list_int(fd, "フォント色", 0, 255),
					act_list_int(fd, "フォント色", 1, 255),
					act_list_int(fd, "フォント色", 2, 255),
					act_float(fd, "フォント太さ", 0.0f),
					act_list_int(fd, "フォント縁取り色", 0, 0),
					act_list_int(fd, "フォント縁取り色", 1, 0),
					act_list_int(fd, "フォント縁取り色", 2, 0),
					act_float(fd, "フォント縁取り", 0.0f), state);
				// 字間隔/行間隔 (char/line spacing) — often negative to tighten the
				// layout (the config sample message uses 行間隔=-5). Set before
				// SetText so the char advances are built with the right spacing.
				PE_SetTextCharSpace(no, act_int(fd, "字間隔", 0), state);
				PE_SetTextLineSpace(no, act_int(fd, "行間隔", 0), state);
			}
			PE_SetText(no, txt, state);
		}
		return;
	}
	// Construction-process viewport (パーツタイプ=18): used as a clip region for
	// preview content (e.g. the config message-window sample). We don't run the
	// full compositing procedure; we only need an opaque mask of the viewport
	// rect so it can serve as an alpha-clipper (rectangular clip) for siblings.
	// The 手順1 create op's 先矩形 gives the size [.,.,.,.,w,h].
	if (act_int(st, "パーツタイプ", -1) == 18) {
		struct ex_tree *proc = act_child(st, "手順リスト");
		struct ex_tree *step1 = proc ? act_child(proc, "手順1") : NULL;
		if (step1) {
			int w = act_list_int(step1, "先矩形", 4, 0);
			int h = act_list_int(step1, "先矩形", 5, 0);
			if (w > 0 && h > 0)
				PE_SetPartsColorFill(no, w, h);
		}
		return;
	}
	struct string *cg = act_str(st, "ＣＧ名");
	if (cg && cg->size)
		PE_SetPartsCG(no, cg, 0, state);
}

// Forward-declared vertical-scrollbar state setters (defined further below).
static void PartsEngine_SetVScrollbarTotalSize(int parts_no, int size);
static void PartsEngine_SetVScrollbarViewSize(int parts_no, int size);
static void PartsEngine_SetVScrollbarScrollRate(int parts_no, float rate);

// Returns the (top-level) parts number built for `node`, or -1 for a leaf/null.
static int act_build_part(struct pe_activity *a, struct ex_tree *node, int parent_no)
{
	if (!node || node->is_leaf)
		return -1;
	int no = ++pe_act_part_seq;

	// register name -> number so the game can resolve it (GetActivityPartsNumber)
	a->parts = xrealloc_array(a->parts, a->nr_parts, a->nr_parts + 1, sizeof(*a->parts));
	struct pe_act_part *ap = &a->parts[a->nr_parts];
	ap->name = string_ref(node->name);
	ap->number = no;
	ap->intent_type = 0;
	ap->intent_dests = NULL;
	ap->nr_intent_dests = 0;
	a->nr_parts++;

	// intent (遷移): 遷移方法 = transition method, 遷移先 = destination list.
	// A part with a transition method acts as an "end/transition" element: when
	// clicked, MouseLClickEvent builds an intent from this and the framework
	// closes/transitions the activity. (BindEndEvent gates on IsExistIntentData.)
	// NOTE: ap is stable here — the recursion below reallocs a->parts, so we must
	// finish filling ap before recursing.
	ap->intent_type = act_int(node, "遷移方法", 0);
	struct ex_tree *dst = act_child(node, "遷移先");
	if (dst && dst->is_leaf && dst->leaf.value.type == EX_LIST) {
		struct ex_list *l = dst->leaf.value.list;
		for (unsigned i = 0; i < l->nr_items; i++) {
			struct ex_value *v = &l->items[i].value;
			if (v->type == EX_STRING && v->s && v->s->size > 0) {
				ap->intent_dests = xrealloc_array(ap->intent_dests,
					ap->nr_intent_dests, ap->nr_intent_dests + 1, sizeof(struct string *));
				ap->intent_dests[ap->nr_intent_dests++] = string_ref(v->s);
			}
		}
	}

	struct ex_tree *ti = act_child(node, "種類別情報");
	int ptype = ti ? act_int(ti, "パーツタイプ", -1) : -1;
	if (getenv("XSYS4_PT_TRACE")) {
		struct string *dcg = ti ? act_str(ti, "ＣＧ名") : NULL;
		NOTICE("PT part '%s' no=%d ptype=%d cg='%s'", display_sjis0(node->name->text),
		       no, ptype, dcg ? display_sjis1(dcg->text) : "(nil)");
	}

	if (ptype == 0 && ti) {
		// button (パーツタイプ=0): the .pactex gives a *base* CG name; the actual
		// per-state assets append a state suffix — normal/hover/down are stored as
		// "<base>／通常", "<base>／オン", "<base>／ダウン" in the CG archive.
		// (state param: 1=DEFAULT/通常, 2=HOVERED/オン, 3=CLICKED/ダウン)
		static const char *const btn_sfx[4] = { NULL, "／通常", "／オン", "／ダウン" };
		struct string *cg = act_str(ti, "ＣＧ名");
		struct string *flat = act_str(ti, "フラット名");
		int bw = act_list_int(ti, "サイズ", 0, 0);
		int bh = act_list_int(ti, "サイズ", 1, 0);
		for (int s = 1; s <= 3; s++) {
			bool have_visual = false;
			if (cg && cg->size) {
				char *sjis = utf2sjis(btn_sfx[s], strlen(btn_sfx[s]));
				struct string *suf = make_string(sjis, strlen(sjis));
				struct string *full = string_concatenate(cg, suf);
				have_visual = PE_SetPartsCG(no, full, 0, s);
				free_string(full);
				free_string(suf);
				free(sjis);
			} else if (flat && flat->size) {
				have_visual = PE_SetPartsFlat(no, flat, s);
			}
			// Fallback hit-area from the declared size ONLY when no CG/flat loaded.
			// (SetPartsRectangleDetectionSize resets the state to RECT_DETECTION,
			// which would erase a CG; a loaded CG provides its own hitbox.)
			if (!have_visual && bw > 0 && bh > 0)
				PE_SetPartsRectangleDetectionSize(no, bw, bh, s);
		}
		PE_SetClickable(no, true);
		PE_SetPartsIsButton(no, true);  // report component type 0 (button) to the game
	} else if (ptype == 3 && ti) {
		// horizontal scrollbar / slider (パーツタイプ=3). ＣＧ名 is a *base* name;
		// the draggable knob ("bar") is stored per-state as "<base>／バー／通常",
		// "<base>／バー／オン", "<base>／バー／ダウン" in the CG archive (the groove
		// "<base>／背景" and the frame CG provide the rail). Track geometry
		// (length/width/total/view/rate) lives here in 種類別情報.
		static const char *const bar_sfx[4] = { NULL, "／バー／通常", "／バー／オン", "／バー／ダウン" };
		struct string *cg = act_str(ti, "ＣＧ名");
		struct string *flat = act_str(ti, "フラット名");
		if (cg && cg->size) {
			for (int s = 1; s <= 3; s++) {
				char *sjis = utf2sjis(bar_sfx[s], strlen(bar_sfx[s]));
				struct string *suf = make_string(sjis, strlen(sjis));
				struct string *full = string_concatenate(cg, suf);
				PE_SetPartsCG(no, full, 0, s);
				free_string(full);
				free_string(suf);
				free(sjis);
			}
		} else if (flat && flat->size) {
			PE_SetPartsFlat(no, flat, 1);
		}
		int base_x = act_list_int(node, "座標", 0, 0);
		int base_y = act_list_int(node, "座標", 1, 0);
		PE_InitPartsHScrollbar(no, base_x, base_y,
			act_int(ti, "長さ", 0), act_int(ti, "幅", 0),
			act_int(ti, "全体スクロール量", 0), act_int(ti, "表示量", 1),
			act_float(ti, "スクロールレート", 0.0f));
		PE_SetClickable(no, true);
	} else if (ptype == 2 && ti) {
		// vertical scrollbar (パーツタイプ=2). Same 種類別情報 geometry as the
		// horizontal slider (全体スクロール量/表示量/スクロールレート). There is no
		// visual vertical-scrollbar compositor yet, but the BACK LOG viewer *reads*
		// GetVScrollbarViewSize to decide how many log lines to instantiate
		// (backlog::detail::CBackLogView@SetLineIndex loops i in 0..ViewSize). With
		// the old no-op stub ViewSize was 0 → zero lines built → empty list. Register
		// the track sizes so the getters return real values. TotalSize is overwritten
		// by the game at runtime (InitVScrollbar sets it to NumofLine); ViewSize comes
		// from the .pactex here.
		int total = act_int(ti, "全体スクロール量", 0);
		int view  = act_int(ti, "表示量", 1);
		float rate = act_float(ti, "スクロールレート", 0.0f);
		PartsEngine_SetVScrollbarTotalSize(no, total);
		PartsEngine_SetVScrollbarViewSize(no, view);
		PartsEngine_SetVScrollbarScrollRate(no, rate);
		// Best-effort visual: load the base CG so the rail shows if present.
		struct string *cg = act_str(ti, "ＣＧ名");
		if (cg && cg->size)
			PE_SetPartsCG(no, cg, 0, 1);
		if (getenv("XSYS4_PT_TRACE"))
			NOTICE("PT vscrollbar no=%d total=%d view=%d rate=%.3f", no, total, view, rate);
		PE_SetClickable(no, true);
	} else if (ptype == 1 && ti) {
		// checkbox (パーツタイプ=1): ＣＧ名 is a base; the box has checked/unchecked
		// variants. テキスト is the label drawn to the right of the box.
		struct string *cg = act_str(ti, "ＣＧ名");
		bool checked = act_int(ti, "チェック状態", 0) != 0;
		if (cg && cg->size) {
			PE_InitPartsCheckBox(no, cg, checked);
		} else {
			// No CG: a colour-swatch button — render a solid fill of its size,
			// tinted by the checkbox colour the game sets (SetCheckBoxColor).
			int sw = act_list_int(ti, "サイズ", 0, 0);
			int sh = act_list_int(ti, "サイズ", 1, 0);
			if (sw > 0 && sh > 0)
				PE_SetPartsColorFill(no, sw, sh);
		}
		PE_SetClickable(no, true);
		struct string *txt = act_str(ti, "テキスト");
		if (txt && txt->size) {
			int base_x = act_list_int(node, "座標", 0, 0);
			int base_y = act_list_int(node, "座標", 1, 0);
			int box_w = PE_GetPartsWidth(no, 1);
			if (box_w <= 0) box_w = act_list_int(ti, "サイズ", 0, 0);
			if (box_w <= 0) box_w = 32;
			int box_h = PE_GetPartsHeight(no, 1);
			int fsize = act_int(ti, "フォントサイズ", 16);
			int tno = ++pe_act_part_seq;
			PE_SetText(tno, txt, 1);
			PE_SetFont(tno, act_int(ti, "フォントタイプ", 0), fsize,
				act_list_int(ti, "フォント色", 0, 255),
				act_list_int(ti, "フォント色", 1, 255),
				act_list_int(ti, "フォント色", 2, 255),
				act_float(ti, "フォント太さ", 0.0f),
				act_list_int(ti, "フォント縁取り色", 0, 0),
				act_list_int(ti, "フォント縁取り色", 1, 0),
				act_list_int(ti, "フォント縁取り色", 2, 0),
				act_float(ti, "フォント縁取り", 0.0f), 1);
			int ty = base_y + (box_h > fsize ? (box_h - fsize) / 2 : 0);
			PE_SetPos(tno, base_x + box_w + 4, ty);
			if (parent_no >= 0)
				PE_SetParentPartsNumber(tno, parent_no);
			PE_SetShow(tno, 1);
		}
	} else if (ti) {
		// CG part: per-state CG names
		act_set_state_cg(no, ti, "通常状態", 1);
		act_set_state_cg(no, ti, "オンカーソル状態", 2);
		act_set_state_cg(no, ti, "キーダウン状態", 3);
	}

	PE_SetPos(no, act_list_int(node, "座標", 0, 0), act_list_int(node, "座標", 1, 0));
	PE_SetZ(no, act_list_int(node, "座標", 2, 0));
	PE_SetAlpha(no, act_int(node, "アルファ", 255));
	// 拡大縮小 (scale x,y): e.g. the config sample-window system icons are 0.5.
	float sx = act_list_float(node, "拡大縮小", 0, 1.0f);
	float sy = act_list_float(node, "拡大縮小", 1, 1.0f);
	if (sx != 1.0f)
		PE_SetPartsMagX(no, sx);
	if (sy != 1.0f)
		PE_SetPartsMagY(no, sy);
	if (act_int(node, "クリック許可", 0))
		PE_SetClickable(no, true);
	if (parent_no >= 0)
		PE_SetParentPartsNumber(no, parent_no);
	PE_SetShow(no, act_int(node, "表示", 1));

	// recurse into child parts. A パーツタイプ=18 "display range" child acts as a
	// clip viewport: subsequent siblings (the preview scene/message-window/icons)
	// are clipped to its rect via the alpha-clipper. This mirrors how the engine
	// confines preview content to a box (used by config message-window previews).
	struct ex_tree *kids = act_child(node, "子パーツ");
	if (kids && !kids->is_leaf) {
		int clip_viewport_no = -1;
		for (unsigned i = 0; i < kids->nr_children; i++) {
			int cno = act_build_part(a, &kids->children[i], no);
			if (cno < 0)
				continue;
			struct ex_tree *cti = act_child(&kids->children[i], "種類別情報");
			struct ex_tree *cnorm = cti ? act_child(cti, "通常状態") : NULL;
			bool is_viewport = cnorm && act_int(cnorm, "パーツタイプ", -1) == 18;
			if (is_viewport)
				clip_viewport_no = cno;
			else if (clip_viewport_no >= 0)
				PE_SetPartsAlphaClipperPartsNumber(cno, clip_viewport_no);
		}
	}
	return no;
}

static bool PE_ReadActivityFile(struct string *activity_name, struct string *file_name)
{
	struct pe_activity *a = pe_act_find(activity_name);
	if (!a) { PE_CreateActivity(activity_name); a = pe_act_find(activity_name); }

	struct archive_data *dfile = asset_get_by_name(ASSET_PACT, file_name->text, NULL);
	if (!dfile) {
		WARNING("ReadActivityFile: '%s' not found in Pact archive", display_sjis0(file_name->text));
		return false;
	}
	struct ex *ex = ex_read(dfile->data, dfile->size);
	archive_free_data(dfile);
	if (!ex) {
		WARNING("ReadActivityFile: failed to parse '%s'", display_sjis0(file_name->text));
		return false;
	}

	char *sjis = utf2sjis("アクティビティ", strlen("アクティビティ"));
	struct ex_value *act = ex_get(ex, sjis);
	free(sjis);
	if (act && act->type == EX_TREE && !act->tree->is_leaf && act->tree->nr_children >= 1) {
		if (getenv("XSYS4_PARTS_TRACE") || getenv("XSYS4_CTRL_TRACE") || getenv("XSYS4_ACT_TRACE")) {
			NOTICE("ReadActivityFile '%s' -> building parts (active ctrl=%d), pre-registered nr_parts=%d",
			       display_sjis0(file_name->text), PE_get_active_controller(), a->nr_parts);
			for (int i = 0; i < a->nr_parts; i++)
				NOTICE("   pre-reg[%d] name='%s' number=%d",
				       i, display_sjis0(a->parts[i].name->text), a->parts[i].number);
		}
		act_build_part(a, &act->tree->children[0], -1);  // ルートパーツ
		if (getenv("XSYS4_CTRL_TRACE") || getenv("XSYS4_DUMP_PARTS")) {
			NOTICE("=== parts right after ReadActivityFile build ===");
			parts_debug_dump();
		}
	}
	ex_free(ex);
	return true;
}

// Activity "end keys" are optional keyboard shortcuts that close an activity
// (e.g. ESC to cancel). Not tracked yet — report none so BindEndType proceeds
// and the activity is driven by its on-screen buttons instead.
static void PE_SetActivityEndKey(struct string *act, int key) { (void)act; (void)key; }
static void PE_EraseActivityEndKey(struct string *act, int key) { (void)act; (void)key; }
static bool PE_IsExistActivityEndKey(struct string *act, int key) { (void)act; (void)key; return false; }
static int PE_NumofActivityEndKey(struct string *act) { (void)act; return 0; }
static int PE_GetActivityEndKey(struct string *act, int index) { (void)act; (void)index; return 0; }

// Recompute transform matrices for the frame. Our renderer derives transforms
// from part params directly, so this is a no-op.
static void PE_UpdateMatrix(bool b) { (void)b; }

// Activity "close parts" (parts whose click closes the activity) and "intent
// data" (data passed between activities) — activity metadata not tracked yet.
// The dialogs we target are driven by their on-screen buttons, so report empty.
static void PE_AddActivityCloseParts(struct string *a, struct string *p) { (void)a; (void)p; }
static void PE_RemoveActivityCloseParts(struct string *a, struct string *p) { (void)a; (void)p; }
static void PE_RemoveAllActivityCloseParts(struct string *a) { (void)a; }
static bool PE_IsExistActivityCloseParts(struct string *a, struct string *p) { (void)a; (void)p; return false; }
static int pe_act_find_by_name(struct pe_activity *a, struct string *pn);

// Intent data is populated by ReadActivityFile from each part's 遷移方法/遷移先.
// The game never calls Set/Add here (it only queries), so those stay no-ops.
static void PE_SetActivityIntentData(struct string *a, struct string *b, struct string *c, int d) { (void)a; (void)b; (void)c; (void)d; }
static void PE_AddActivityIntentDataDestination(struct string *a, struct string *b, struct string *c) { (void)a; (void)b; (void)c; }

static struct pe_act_part *pe_act_intent_part(struct string *act, struct string *pn)
{
	struct pe_activity *a = pe_act_find(act);
	int idx = pe_act_find_by_name(a, pn);
	return idx >= 0 ? &a->parts[idx] : NULL;
}

// A part "has intent data" iff it declares a transition method (遷移方法 != 0).
// BindEndEvent gates MouseLClickEvent registration on this, so it must be true
// for buttons that close/transition the activity.
static bool PE_IsExistActivityIntentData(struct string *act, struct string *pn)
{
	struct pe_act_part *p = pe_act_intent_part(act, pn);
	bool r = p && p->intent_type != 0;
	if (getenv("XSYS4_ACT_TRACE"))
		NOTICE("ACT IsExistActivityIntentData(act='%s', name='%s') -> %d",
		       display_sjis0(act->text), display_sjis1(pn->text), r);
	return r;
}
static int PE_NumofActivityIntentDataDestination(struct string *act, struct string *pn)
{
	struct pe_act_part *p = pe_act_intent_part(act, pn);
	return p ? p->nr_intent_dests : 0;
}
static void PE_GetActivityIntentDataDestination(struct string **out, int i, struct string *act, struct string *pn)
{
	struct pe_act_part *p = pe_act_intent_part(act, pn);
	if (*out) free_string(*out);
	*out = string_ref(p && i >= 0 && i < p->nr_intent_dests ? p->intent_dests[i] : &EMPTY_STRING);
}
static int PE_GetActivityIntentDataType(struct string *act, struct string *pn)
{
	struct pe_act_part *p = pe_act_intent_part(act, pn);
	return p ? p->intent_type : 0;
}

static int pe_act_find_by_name(struct pe_activity *a, struct string *pn)
{
	for (int i = 0; a && i < a->nr_parts; i++)
		if (!strcmp(a->parts[i].name->text, pn->text))
			return i;
	return -1;
}

static bool PE_RemoveActivityParts(struct string *act, struct string *parts_name)
{
	struct pe_activity *a = pe_act_find(act);
	int idx = pe_act_find_by_name(a, parts_name);
	if (idx < 0)
		return false;
	free_string(a->parts[idx].name);
	memmove(&a->parts[idx], &a->parts[idx+1], (a->nr_parts - idx - 1) * sizeof(*a->parts));
	a->nr_parts--;
	return true;
}

static bool PE_RemoveAllActivityParts(struct string *act)
{
	struct pe_activity *a = pe_act_find(act);
	if (!a)
		return false;
	for (int i = 0; i < a->nr_parts; i++)
		free_string(a->parts[i].name);
	a->nr_parts = 0;
	return true;
}

static int PE_NumofActivityParts(struct string *act)
{
	struct pe_activity *a = pe_act_find(act);
	return a ? a->nr_parts : 0;
}

static bool PE_GetActivityParts(int index, struct string *act, struct string **name, int *number)
{
	struct pe_activity *a = pe_act_find(act);
	if (!a || index < 0 || index >= a->nr_parts)
		return false;
	if (*name) free_string(*name);
	*name = string_ref(a->parts[index].name);
	*number = a->parts[index].number;
	return true;
}

static bool PE_IsExistActivityPartsByName(struct string *act, struct string *pn)
{
	return pe_act_find_by_name(pe_act_find(act), pn) >= 0;
}

static bool PE_IsExistActivityPartsByNumber(struct string *act, int number)
{
	struct pe_activity *a = pe_act_find(act);
	for (int i = 0; a && i < a->nr_parts; i++)
		if (a->parts[i].number == number)
			return true;
	return false;
}

static int PE_GetActivityPartsNumber(struct string *act, struct string *pn)
{
	struct pe_activity *a = pe_act_find(act);
	int idx = pe_act_find_by_name(a, pn);
	int r = idx >= 0 ? a->parts[idx].number : -1;
	if (getenv("XSYS4_ACT_TRACE"))
		NOTICE("ACT GetActivityPartsNumber(act='%s', name='%s') -> %d",
		       display_sjis0(act->text), display_sjis1(pn->text), r);
	return r;
}

static void PE_GetActivityPartsName(struct string **out, struct string *act, int number)
{
	struct pe_activity *a = pe_act_find(act);
	if (*out) free_string(*out);
	*out = NULL;
	for (int i = 0; a && i < a->nr_parts; i++) {
		if (a->parts[i].number == number) {
			*out = string_ref(a->parts[i].name);
			return;
		}
	}
	*out = string_ref(&EMPTY_STRING);
}

static void PE_SetActivityEXText(struct string *act, struct string *text)
{
	struct pe_activity *a = pe_act_find(act);
	if (!a) { PE_CreateActivity(act); a = pe_act_find(act); }
	if (a->ex_text) free_string(a->ex_text);
	a->ex_text = string_ref(text);
}

static void PE_GetActivityEXText(struct string *act, struct string **out)
{
	struct pe_activity *a = pe_act_find(act);
	if (*out) free_string(*out);
	*out = string_ref(a && a->ex_text ? a->ex_text : &EMPTY_STRING);
}

static int PE_GetActivityEXID(struct string *act)
{
	struct pe_activity *a = pe_act_find(act);
	return a ? a->ex_id : -1;
}

// Проверка наличия файла активности: ищем запись в Pact-архиве так же, как это
// делает ReadActivityFile. Без этого CConfigView@Execute (и другие экраны-меню)
// получают пустое имя активности и рисуют чёрный экран.
static bool PE_IsExistActivityFile(struct string *a)
{
	if (!a || !a->text || !a->text[0])
		return false;
	struct archive_data *dfile = asset_get_by_name(ASSET_PACT, a->text, NULL);
	if (!dfile)
		return false;
	archive_free_data(dfile);
	return true;
}

// Остальные файловые операции активностей — не поддержаны: безопасные заглушки.
HLL_QUIET_UNIMPLEMENTED(true,  bool, PartsEngine, WriteActivityFile, struct string *a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(false, bool, PartsEngine, SaveActivityEXText, struct string **a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(false, bool, PartsEngine, LoadActivityEXText, struct string *a, struct string *b);

// --- Button-виджеты (GUI-тулкит новых игр). Внешний вид пока не реализуем;
// сеттеры — no-op, геттеры — разумные дефолты. Кнопки считаем включёнными,
// чтобы логика меню могла по ним кликать. ---
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonSize, int a, int b, int c);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonEnable, int a, bool b);
HLL_QUIET_UNIMPLEMENTED(true, bool, PartsEngine, IsButtonEnable, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonColor, int a, int b, int c, int d);
HLL_QUIET_UNIMPLEMENTED(255, int, PartsEngine, GetButtonR, int a);
HLL_QUIET_UNIMPLEMENTED(255, int, PartsEngine, GetButtonG, int a);
HLL_QUIET_UNIMPLEMENTED(255, int, PartsEngine, GetButtonB, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonFontProperty, int a, int b, int c, int d, int e, int f, float g, int h, int i, int j, float k);
// ВАЖНО: ref-output геттеры ОБЯЗАНЫ заполнять выходы — иначе игра читает
// неинициализированный локал (мусор INT_MIN) и, напр., сохраняет как размер
// шрифта → текст не рисуется. Пишем разумные дефолты (размер шрифта 16).
static void PE_GetButtonFontProperty(int a, int *type, int *size, int *r, int *g, int *b,
	float *weight, int *sr, int *sg, int *sb, float *sw) {
	(void)a;
	if (type) *type = 0;
	if (size) *size = 16;
	if (r) *r = 255; if (g) *g = 255; if (b) *b = 255;
	if (weight) *weight = 1.0f;
	if (sr) *sr = 0; if (sg) *sg = 0; if (sb) *sb = 0;
	if (sw) *sw = 0.0f;
}
static void PE_str_out(struct string **out) {
	if (*out) free_string(*out);
	*out = string_ref(&EMPTY_STRING);
}
static void PE_GetButtonCGName(int a, struct string **b) { (void)a; PE_str_out(b); }
static void PE_GetButtonFlatName(int a, struct string **b) { (void)a; PE_str_out(b); }
static void PE_GetButtonText(int a, struct string **b) { (void)a; PE_str_out(b); }

// GetPartsTextFontProperty: геттер свойств шрифта текст-парта (форма как у
// GetButtonFontProperty + хвостовой State). Полноценного хранилища свойств
// текст-парта в движке нет — возвращаем разумные дефолты, чтобы вызывающий код
// (диалоги/бэклог) не читал мусор из ref-out и не падал.
static void PE_GetPartsTextFontProperty(int a, int *type, int *size, int *r, int *g, int *b,
	float *weight, int *er, int *eg, int *eb, float *ew, int state) {
	// Sensible defaults first (ref-out getters MUST fill outputs, else callers read
	// garbage — e.g. a bad size leaves text unrendered).
	if (type) *type = 0;
	if (size) *size = 16;
	if (r) *r = 255; if (g) *g = 255; if (b) *b = 255;
	if (weight) *weight = 1.0f;
	if (er) *er = 0; if (eg) *eg = 0; if (eb) *eb = 0;
	if (ew) *ew = 0.0f;
	// Tsumamigui 3 reads THIS on the message-window text parts to pick the BACK LOG
	// font size, then builds the log construction ops at the returned size. Return
	// the part's ACTUAL font SIZE so the log matches the message window (30) instead
	// of a hardcoded 16. We deliberately take ONLY the size: the part's internal
	// ts.face is an engine face id (e.g. 256), not the HLL font TYPE the caller
	// expects, and feeding it back as `type` makes the game build the log in an
	// unrenderable font (blank log). Type/color/weight stay at the safe defaults.
	int real_size = 16;
	PE_GetTextFontProps(a, state, NULL, &real_size, NULL, NULL, NULL, NULL);
	if (size) *size = real_size;
}
// Фокус ввода (клавиатурная навигация по кнопкам) в движке не реализован —
// косметика. Явные заглушки, видимые в исходнике.
static int PE_GetFocusPartsNumber(void) { return -1; }
static bool PE_IsFocus(int a) { (void)a; return false; }
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetFocus, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetFocusPartsNumber, int a);

// Button parts are backed by the normal parts state machine (3 states:
// default/hovered/clicked). Setting a button's flat/CG name must actually
// load the resource into all states so the button renders — otherwise the
// title menu (built entirely from flat buttons) stays invisible (black).
static void PE_SetButtonCGName(int parts_no, struct string *name) {
	for (int st = 1; st <= 3; st++)  // default/hovered/clicked
		PE_SetPartsCG(parts_no, name, 0, st);
}
static void PE_SetButtonFlatName(int parts_no, struct string *name) {
	for (int st = 1; st <= 3; st++)  // default/hovered/clicked
		PE_SetPartsFlat(parts_no, name, st);
}

// Newer movie-parts API (CreatePartsMovie/PlayPartsMovie/IsEndPartsMovie/
// ReleasePartsMovie). Video playback for these is not implemented yet, so we
// treat the movie as instantly finished — this SKIPS the OP movie so the game
// proceeds to the title menu instead of waiting forever on a black screen.
static bool PE_CreatePartsMovie(int parts_no, struct string *filename, int a, int b, int c, int d, int e, int f) {
	(void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
	if (getenv("XSYS4_PARTS_TRACE"))
		NOTICE("PARTS CreatePartsMovie(%d, '%s') [skip]", parts_no,
		       filename ? filename->text : "(null)");
	return true;
}
static bool PE_PlayPartsMovie(int parts_no, int state) {
	(void)state;
	if (getenv("XSYS4_PARTS_TRACE"))
		NOTICE("PARTS PlayPartsMovie(%d) [skip]", parts_no);
	return true;
}
static bool PE_IsEndPartsMovie(int parts_no, int state) {
	(void)parts_no; (void)state;
	return true;  // report finished immediately so the OP is skipped
}
static bool PE_ReleasePartsMovie(int parts_no, int state) {
	(void)parts_no; (void)state;
	return true;
}
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonText, int a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonTextOriginPosMode, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetButtonTextOriginPosMode, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonCharSpace, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetButtonCharSpace, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonLineSpace, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetButtonLineSpace, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetCheckBoxButtonWidth, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetCheckBoxButtonHeight, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetCheckBoxButtonMode, int a, bool b);
HLL_QUIET_UNIMPLEMENTED(false, bool, PartsEngine, IsCheckBoxButtonMode, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarMoveSizeByButton, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetVScrollbarMoveSizeByButton, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarMoveSizeByButton, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarMoveSizeByButton, int a);
// Скроллбары (BACK LOG и пр.): вертикального скроллбара в parts пока нет
// (горизонтальный-слайдер есть — PE_*HScrollbarScrollRate). Тихие заглушки,
// чтобы UI открывался без падения в REPL; сам ползунок не отрисовывается.
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarOnCursorSoundNumber, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarClickSoundNumber, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetVScrollbarOnCursorSoundNumber, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetVScrollbarClickSoundNumber, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarSize, int a, int b, int c);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarUpHeight, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarDownHeight, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetVScrollbarUpHeight, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetVScrollbarDownHeight, int a);
// --- Вертикальный скроллбар: минимальная модель состояния (total/view/pos),
// keyed по номеру parts. Нужна бэклогу Tsumamigui 3: CBackLogView@SetLineIndex
// строит видимые строки в цикле «for i in 0..GetVScrollbarViewSize()»; при
// заглушке-0 не создавалось ни одной строки → пустой список. Отрисовку самого
// ползунка не трогаем (её и не было); модель нужна для логики прокрутки/выдачи. ---
// Declared in parts_internal.h (not included here); needed to post a Scroll
// message when the scrollbar position changes.
struct parts *parts_try_get(int parts_no);
void parts_msg_push(struct parts *parts, int type, const char *fmt, ...);
#define PE_PARTS_MSG_SCROLL 20  // == PARTS_MSG_SCROLL (game CallDelegate case 19 + 1)

struct pe_vscrollbar {
	int parts_no;
	int total_size;
	int view_size;
	int scroll_pos;
};
static struct pe_vscrollbar *pe_vscrollbars = NULL;
static int nr_pe_vscrollbars = 0;

static struct pe_vscrollbar *pe_vscrollbar_get(int parts_no, bool create)
{
	for (int i = 0; i < nr_pe_vscrollbars; i++)
		if (pe_vscrollbars[i].parts_no == parts_no)
			return &pe_vscrollbars[i];
	if (!create)
		return NULL;
	pe_vscrollbars = xrealloc_array(pe_vscrollbars, nr_pe_vscrollbars,
			nr_pe_vscrollbars + 1, sizeof(*pe_vscrollbars));
	struct pe_vscrollbar *sb = &pe_vscrollbars[nr_pe_vscrollbars++];
	*sb = (struct pe_vscrollbar){ .parts_no = parts_no };
	return sb;
}

static void PartsEngine_SetVScrollbarTotalSize(int parts_no, int size)
{
	pe_vscrollbar_get(parts_no, true)->total_size = size;
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("VScrollbar[%d] TotalSize=%d", parts_no, size);
}
static void PartsEngine_SetVScrollbarViewSize(int parts_no, int size)
{
	pe_vscrollbar_get(parts_no, true)->view_size = size;
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("VScrollbar[%d] ViewSize=%d", parts_no, size);
}
static void PartsEngine_SetVScrollbarScrollPos(int parts_no, int pos)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, true);
	int max = sb->total_size - sb->view_size;
	if (max < 0) max = 0;
	if (pos < 0) pos = 0;
	if (pos > max) pos = max;
	sb->scroll_pos = pos;
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("VScrollbar[%d] SetScrollPos req=%d -> %d (total=%d view=%d max=%d)",
		       parts_no, pos, sb->scroll_pos, sb->total_size, sb->view_size, max);
	// Notify the game's scroll delegate (e.g. backlog SetLineIndex) that the bar
	// moved. The real engine posts a Scroll message when a scrollbar's position is
	// set; the game's CPartsMessageManager dispatches it to the registered
	// ScrollEvent. Post (ScrollPos, TotalSize) UNCONDITIONALLY — not only on change.
	// The backlog's InitVScrollbar sets ScrollPos to NumofLine to trigger the very
	// first SetLineIndex via ScrollEvent; with a small history (NumofLine <= ViewSize)
	// the clamped pos stays 0 == old, so gating on change suppressed the initial
	// render and the log stayed empty until enough lines accrued (>ViewSize). No feedback
	// loop: SetLineIndex never calls back into SetVScrollbarScrollPos.
	struct parts *p = parts_try_get(parts_no);
	if (p)
		parts_msg_push(p, PE_PARTS_MSG_SCROLL, "ii", pos, sb->total_size);
}
static void PartsEngine_SetVScrollbarScrollRate(int parts_no, float rate)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, true);
	int max = sb->total_size - sb->view_size;
	if (max < 0) max = 0;
	if (rate < 0.0f) rate = 0.0f;
	if (rate > 1.0f) rate = 1.0f;
	sb->scroll_pos = (int)(rate * max + 0.5f);
}
static int PartsEngine_GetVScrollbarTotalSize(int parts_no)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, false);
	return sb ? sb->total_size : 0;
}
static int PartsEngine_GetVScrollbarViewSize(int parts_no)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, false);
	int v = sb ? sb->view_size : 0;
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("VScrollbar[%d] GetViewSize -> %d", parts_no, v);
	return v;
}
static int PartsEngine_GetVScrollbarScrollPos(int parts_no)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, false);
	int p = sb ? sb->scroll_pos : 0;
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("VScrollbar[%d] GetScrollPos -> %d", parts_no, p);
	return p;
}
static float PartsEngine_GetVScrollbarScrollRate(int parts_no)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, false);
	if (!sb) return 0.0f;
	int max = sb->total_size - sb->view_size;
	if (max <= 0) return 0.0f;
	return (float)sb->scroll_pos / (float)max;
}
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarCGName, int a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, GetVScrollbarCGName, int a, struct string **b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarFlatName, int a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, GetVScrollbarFlatName, int a, struct string **b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarOnCursorSoundNumber, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarClickSoundNumber, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarOnCursorSoundNumber, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarClickSoundNumber, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarSize, int a, int b, int c);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarLeftWidth, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarRightWidth, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarLeftWidth, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarRightWidth, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarTotalSize, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarViewSize, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarScrollPos, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarTotalSize, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarViewSize, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarScrollPos, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarCGName, int a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, GetHScrollbarCGName, int a, struct string **b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarFlatName, int a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, GetHScrollbarFlatName, int a, struct string **b);
HLL_QUIET_UNIMPLEMENTED(false, bool, PartsEngine, IsRadioButtonBoxExistGUI, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, ClearRadioButtonBoxChild, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, AddRadioButtonBoxChild, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, RemoveRadioButtonBoxChild, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, NumofRadioButtonBoxChild, int a);
HLL_QUIET_UNIMPLEMENTED(-1, int, PartsEngine, GetRadioButtonBoxChild, int a, int b);

// --- Дочерние parts. Иерархия (для рендера) через готовый parent-указатель;
// запросы количества/по-индексу минимальны. ---
static void PE_AddChild(int parent, int child) { PE_SetParentPartsNumber(child, parent); }
static void PE_InsertChild(int parent, int child, int index) { (void)index; PE_SetParentPartsNumber(child, parent); }
static void PE_RemoveChild(int parent, int child) { (void)parent; PE_SetParentPartsNumber(child, -1); }
static bool PE_IsExistChild(int parent, int child) { return PE_GetParentPartsNumber(child) == parent; }
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, ClearChild, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, NumofChild, int a);
HLL_QUIET_UNIMPLEMENTED(-1, int, PartsEngine, GetChild, int a, int b);
HLL_QUIET_UNIMPLEMENTED(-1, int, PartsEngine, GetChildIndex, int a, int b);
// Ixseal (Healing Touch/Dohna): SetEventID(parts, ?, event_id) — привязка id
// события ввода к части; для достижения экрана безвредный no-op.
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetEventID, int a, int b, int c);
// Parts_StopSwipe() — отменяет свайп-инерцию. Зовётся первым в
// CBackLogView@MouseWheelEvent (и swipe-обработчиках). Свайп-инерцию не
// моделируем, поэтому no-op; без него колесо в бэклоге падало в REPL.
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, Parts_StopSwipe, void);
// Ixseal CParts@Comment: attaches a debug/author comment string to a part.
// Purely diagnostic metadata — safe no-op for reaching the screen.
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, Parts_SetComment, int a, struct string *b);

static void PartsEngine_PreLink(void);

HLL_LIBRARY(PartsEngine,
	    HLL_EXPORT(_PreLink, PartsEngine_PreLink),
	    // for versions without PartsEngine.Init
	    HLL_EXPORT(_ModuleInit, PartsEngine_ModuleInit),
	    HLL_EXPORT(_ModuleFini, PartsEngine_ModuleFini),
	    // Oyako Rankan
	    HLL_EXPORT(Init, PE_Init),
	    HLL_EXPORT(Update, PartsEngine_Update),
	    HLL_TODO_EXPORT(UpdateGUIController, PartsEngine_UpdateGUIController),
	    HLL_EXPORT(SetWantSaveBackScene, PE_SetWantSaveBackScene),
	    HLL_EXPORT(SaveBackScene, PE_SaveBackScene),
	    HLL_EXPORT(SaveThumbnail, PE_SaveThumbnail),
	    HLL_EXPORT(GetFreeSystemPartsNumber, PE_GetFreeNumber),
	    // FIXME: what is the difference?
	    HLL_EXPORT(GetFreeSystemPartsNumberNotSaved, PE_GetFreeNumber),
	    HLL_EXPORT(IsExistParts, PE_IsExist),
	    HLL_EXPORT(SetPartsCG, PE_SetPartsCG),
	    HLL_EXPORT(CreatePartsMovie, PE_CreatePartsMovie),
	    HLL_EXPORT(PlayPartsMovie, PE_PlayPartsMovie),
	    HLL_EXPORT(IsEndPartsMovie, PE_IsEndPartsMovie),
	    HLL_EXPORT(ReleasePartsMovie, PE_ReleasePartsMovie),
	    HLL_EXPORT(GetPartsCGName, PE_GetPartsCGName),
	    HLL_EXPORT(SetPartsCGSurfaceArea, PE_SetPartsCGSurfaceArea),
	    HLL_EXPORT(SetLoopCG, PE_SetLoopCG),
	    HLL_EXPORT(SetLoopCGSurfaceArea, PE_SetLoopCGSurfaceArea),
	    HLL_EXPORT(SetText, PE_SetText),
	    HLL_EXPORT(AddPartsText, PE_AddPartsText),
	    HLL_EXPORT(GetTextPartsText, PE_GetTextPartsText),
	    HLL_TODO_EXPORT(DeletePartsTopTextLine, PartsEngine_DeletePartsTopTextLine),
	    HLL_EXPORT(SetPartsTextSurfaceArea, PE_SetPartsTextSurfaceArea),
	    HLL_TODO_EXPORT(SetPartsTextHighlight, PartsEngine_SetPartsTextHighlight),
	    HLL_TODO_EXPORT(AddPartsTextHighlight, PartsEngine_AddPartsTextHighlight),
	    HLL_TODO_EXPORT(ClearPartsTextHighlight, PartsEngine_ClearPartsTextHighlight),
	    HLL_TODO_EXPORT(SetPartsTextCountReturn, PartsEngine_SetPartsTextCountReturn),
	    HLL_TODO_EXPORT(GetPartsTextCountReturn, PartsEngine_GetPartsTextCountReturn),
	    HLL_EXPORT(SetFont, PE_SetFont),
	    HLL_EXPORT(SetPartsFontType, PE_SetPartsFontType),
	    HLL_EXPORT(SetPartsFontSize, PE_SetPartsFontSize),
	    HLL_EXPORT(SetPartsFontColor, PE_SetPartsFontColor),
	    HLL_EXPORT(SetPartsFontBoldWeight, PE_SetPartsFontBoldWeight),
	    HLL_EXPORT(SetPartsFontEdgeColor, PE_SetPartsFontEdgeColor),
	    HLL_EXPORT(SetPartsFontEdgeWeight, PE_SetPartsFontEdgeWeight),
	    HLL_EXPORT(SetTextCharSpace, PE_SetTextCharSpace),
	    HLL_EXPORT(SetTextLineSpace, PE_SetTextLineSpace),
	    HLL_EXPORT(GetTextCharSpace, PE_GetTextCharSpace),
	    HLL_EXPORT(GetTextLineSpace, PE_GetTextLineSpace),
	    HLL_EXPORT(SetHGaugeCG, PE_SetHGaugeCG),
	    HLL_EXPORT(SetHGaugeRate, PE_SetHGaugeRate_int),
	    HLL_EXPORT(SetVGaugeCG, PE_SetVGaugeCG),
	    HLL_EXPORT(SetVGaugeRate, PE_SetVGaugeRate_int),
	    HLL_EXPORT(SetHGaugeSurfaceArea, PE_SetHGaugeSurfaceArea),
	    HLL_EXPORT(SetVGaugeSurfaceArea, PE_SetVGaugeSurfaceArea),
	    HLL_EXPORT(SetNumeralCG, PE_SetNumeralCG),
	    HLL_EXPORT(SetNumeralLinkedCGNumberWidthWidthList, PE_SetNumeralLinkedCGNumberWidthWidthList),
	    HLL_TODO_EXPORT(SetNumeralFont, PartsEngine_SetNumeralFont),
	    HLL_EXPORT(SetNumeralNumber, PE_SetNumeralNumber),
	    HLL_EXPORT(SetNumeralShowComma, PE_SetNumeralShowComma),
	    HLL_EXPORT(SetNumeralSpace, PE_SetNumeralSpace),
	    HLL_EXPORT(SetNumeralLength, PE_SetNumeralLength),
	    HLL_EXPORT(SetNumeralSurfaceArea, PE_SetNumeralSurfaceArea),
	    HLL_EXPORT(SetPartsRectangleDetectionSize, PE_SetPartsRectangleDetectionSize),
	    HLL_TODO_EXPORT(SetPartsRectangleDetectionSurfaceArea, PartsEngine_SetPartsRectangleDetectionSurfaceArea),
	    HLL_EXPORT(SetPartsCGDetectionSize, PE_SetPartsCGDetectionSize),
	    HLL_TODO_EXPORT(SetPartsCGDetectionSurfaceArea, PartsEngine_SetPartsCGDetectionSurfaceArea),
	    HLL_EXPORT(SetPartsFlash, PE_SetPartsFlash),
	    HLL_EXPORT(IsPartsFlashEnd, PE_IsPartsFlashEnd),
	    HLL_EXPORT(GetPartsFlashCurrentFrameNumber, PE_GetPartsFlashCurrentFrameNumber),
	    HLL_EXPORT(BackPartsFlashBeginFrame, PE_BackPartsFlashBeginFrame),
	    HLL_EXPORT(StepPartsFlashFinalFrame, PE_StepPartsFlashFinalFrame),
	    HLL_TODO_EXPORT(SetPartsFlashSurfaceArea, PE_SetPartsFlashSurfaceArea),
	    HLL_EXPORT(SetPartsFlashAndStop, PE_SetPartsFlashAndStop),
	    HLL_EXPORT(StopPartsFlash, PE_StopPartsFlash),
	    HLL_EXPORT(StartPartsFlash, PE_StartPartsFlash),
	    HLL_EXPORT(GoFramePartsFlash, PE_GoFramePartsFlash),
	    HLL_EXPORT(GetPartsFlashEndFrame, PE_GetPartsFlashEndFrame),
	    HLL_EXPORT(ExistsFlashFile, PE_ExistsFlashFile),
	    HLL_EXPORT(ClearPartsConstructionProcess, PE_ClearPartsConstructionProcess),
	    HLL_EXPORT(AddCreateToPartsConstructionProcess, PE_AddCreateToPartsConstructionProcess),
	    HLL_EXPORT(AddCreatePixelOnlyToPartsConstructionProcess, PE_AddCreatePixelOnlyToPartsConstructionProcess),
	    HLL_EXPORT(AddCreateCGToProcess, PE_AddCreateCGToProcess),
	    HLL_EXPORT(AddFillToPartsConstructionProcess, PE_AddFillToPartsConstructionProcess),
	    HLL_EXPORT(AddFillAlphaColorToPartsConstructionProcess, PE_AddFillAlphaColorToPartsConstructionProcess),
	    HLL_EXPORT(AddFillAMapToPartsConstructionProcess, PE_AddFillAMapToPartsConstructionProcess),
	    HLL_EXPORT(AddFillWithAlphaToPartsConstructionProcess, PE_AddFillWithAlphaToPartsConstructionProcess),
	    HLL_TODO_EXPORT(AddFillGradationHorizonToPartsConstructionProcess, PartsEngine_AddFillGradationHorizonToPartsConstructionProcess),
	    HLL_EXPORT(AddDrawRectToPartsConstructionProcess, PE_AddDrawRectToPartsConstructionProcess),
	    HLL_EXPORT(AddDrawCutCGToPartsConstructionProcess, PartsEngine_AddDrawCutCGToPartsConstructionProcess_old),
	    HLL_EXPORT(AddCopyCutCGToPartsConstructionProcess, PartsEngine_AddCopyCutCGToPartsConstructionProcess_old),
	    HLL_EXPORT(AddGrayFilterToPartsConstructionProcess, PE_AddGrayFilterToPartsConstructionProcess),
	    HLL_TODO_EXPORT(AddAddFilterToPartsConstructionProcess, PartsEngine_AddAddFilterToPartsConstructionProcess),
	    HLL_TODO_EXPORT(AddMulFilterToPartsConstructionProcess, PartsEngine_AddMulFilterToPartsConstructionProcess),
	    HLL_EXPORT(BuildPartsConstructionProcess, PE_BuildPartsConstructionProcess),
	    HLL_EXPORT(AddDrawTextToPartsConstructionProcess, PE_AddDrawTextToPartsConstructionProcess),
	    HLL_EXPORT(AddCopyTextToPartsConstructionProcess, PE_AddCopyTextToPartsConstructionProcess),
	    HLL_EXPORT(SetPartsConstructionSurfaceArea, PE_SetPartsConstructionSurfaceArea),
	    HLL_EXPORT(ReleaseParts, PE_ReleaseParts),
	    HLL_EXPORT(ReleaseAllParts, PE_ReleaseAllParts),
	    HLL_EXPORT(ReleaseAllPartsWithoutSystem, PE_ReleaseAllPartsWithoutSystem),
	    HLL_EXPORT(SetPos, PE_SetPos),
	    HLL_EXPORT(SetZ, PE_SetZ),
	    HLL_EXPORT(SetShow, PE_SetShow),
	    HLL_EXPORT(SetAlpha, PE_SetAlpha),
	    HLL_EXPORT(SetPartsDrawFilter, PE_SetPartsDrawFilter),
	    HLL_EXPORT(SetAddColor, PE_SetAddColor),
	    HLL_EXPORT(SetMultiplyColor, PE_SetMultiplyColor),
	    HLL_EXPORT(SetPassCursor, PE_SetPassCursor),
	    HLL_EXPORT(SetClickable, PE_SetClickable),
	    HLL_EXPORT(SetSpeedupRateByMessageSkip, PE_SetSpeedupRateByMessageSkip),
	    HLL_TODO_EXPORT(SetResetTimerByChangeInputStatus, PartsEngine_SetResetTimerByChangeInputStatus),
	    HLL_EXPORT(GetPartsX, PE_GetPartsX),
	    HLL_EXPORT(GetPartsY, PE_GetPartsY),
	    HLL_EXPORT(GetPartsZ, PE_GetPartsZ),
	    HLL_EXPORT(GetPartsShow, PE_GetPartsShow),
	    HLL_EXPORT(GetPartsAlpha, PE_GetPartsAlpha),
	    HLL_TODO_EXPORT(GetAddColor, PartsEngine_GetAddColor),
	    HLL_TODO_EXPORT(GetMultiplyColor, PartsEngine_GetMultiplyColor),
	    HLL_EXPORT(GetPartsPassCursor, PE_GetPartsPassCursor),
	    HLL_EXPORT(GetPartsClickable, PE_GetPartsClickable),
	    HLL_TODO_EXPORT(GetPartsSpeedupRateByMessageSkip, PartsEngine_GetPartsSpeedupRateByMessageSkip),
	    HLL_TODO_EXPORT(GetResetTimerByChangeInputStatus, PartsEngine_GetResetTimerByChangeInputStatus),
	    HLL_EXPORT(GetPartsUpperLeftPosX, PE_GetPartsUpperLeftPosX),
	    HLL_EXPORT(GetPartsUpperLeftPosY, PE_GetPartsUpperLeftPosY),
	    HLL_EXPORT(GetPartsWidth, PE_GetPartsWidth),
	    HLL_EXPORT(GetPartsHeight, PE_GetPartsHeight),
	    HLL_EXPORT(SetInputState, PE_SetInputState),
	    HLL_EXPORT(GetInputState, PE_GetInputState),
	    HLL_EXPORT(SetPartsOriginPosMode, PE_SetPartsOriginPosMode),
	    HLL_EXPORT(GetPartsOriginPosMode, PE_GetPartsOriginPosMode),
	    HLL_EXPORT(SetParentPartsNumber, PE_SetParentPartsNumber),
	    HLL_EXPORT(Parts_GetParentPartsNumber, PE_GetParentPartsNumber),
	    HLL_EXPORT(SetPartsGroupNumber, PE_SetPartsGroupNumber),
	    HLL_EXPORT(SetPartsGroupDecideOnCursor, PE_SetPartsGroupDecideOnCursor),
	    HLL_EXPORT(SetPartsGroupDecideClick, PE_SetPartsGroupDecideClick),
	    HLL_EXPORT(SetOnCursorShowLinkPartsNumber, PE_SetOnCursorShowLinkPartsNumber),
	    HLL_EXPORT(SetPartsMessageWindowShowLink, PE_SetPartsMessageWindowShowLink),
	    HLL_EXPORT(GetPartsMessageWindowShowLink, PE_GetPartsMessageWindowShowLink),
	    HLL_EXPORT(AddMotionPos, PE_AddMotionPos_curve),
	    HLL_EXPORT(AddMotionAlpha, PE_AddMotionAlpha_curve),
	    HLL_TODO_EXPORT(AddMotionCG, PartsEngine_AddMotionCG),
	    HLL_EXPORT(AddMotionHGaugeRate, PE_AddMotionHGaugeRate_curve),
	    HLL_EXPORT(AddMotionVGaugeRate, PE_AddMotionVGaugeRate_curve),
	    HLL_EXPORT(AddMotionNumeralNumber, PE_AddMotionNumeralNumber_curve),
	    HLL_EXPORT(AddMotionMagX, PE_AddMotionMagX_curve),
	    HLL_EXPORT(AddMotionMagY, PE_AddMotionMagY_curve),
	    HLL_EXPORT(AddMotionRotateX, PE_AddMotionRotateX_curve),
	    HLL_EXPORT(AddMotionRotateY, PE_AddMotionRotateY_curve),
	    HLL_EXPORT(AddMotionRotateZ, PE_AddMotionRotateZ_curve),
	    HLL_EXPORT(AddMotionVibrationSize, PE_AddMotionVibrationSize),
	    HLL_EXPORT(AddWholeMotionVibrationSize, PE_AddWholeMotionVibrationSize),
	    HLL_EXPORT(AddMotionSound, PE_AddMotionSound),
	    HLL_TODO_EXPORT(SetSoundNumber, PartsEngine_SetSoundNumber),
	    HLL_TODO_EXPORT(GetSoundNumber, PartsEngine_GetSoundNumber),
	    HLL_EXPORT(SetClickMissSoundNumber, PE_SetClickMissSoundNumber),
	    HLL_TODO_EXPORT(GetClickMissSoundNumber, PartsEngine_GetClickMissSoundNumber),
	    HLL_EXPORT(BeginMotion, PE_BeginMotion),
	    HLL_EXPORT(EndMotion, PE_EndMotion),
	    HLL_EXPORT(PauseMotion, PE_PauseMotion),
	    HLL_EXPORT(IsMotion, PE_IsMotion),
	    HLL_EXPORT(SeekEndMotion, PE_SeekEndMotion),
	    HLL_EXPORT(UpdateMotionTime, PE_UpdateMotionTime),
	    HLL_EXPORT(BeginInput, PE_BeginInput),
	    HLL_EXPORT(EndInput, PE_EndInput),
	    HLL_EXPORT(GetClickPartsNumber, PE_GetClickPartsNumber),
	    HLL_EXPORT(GetFocusPartsNumber, PE_GetFocusPartsNumber),
	    HLL_EXPORT(SetFocusPartsNumber, PartsEngine_SetFocusPartsNumber),
	    HLL_EXPORT(GetPartsTextFontProperty, PE_GetPartsTextFontProperty),
	    HLL_TODO_EXPORT(PushGUIController, PartsEngine_PushGUIController),
	    HLL_TODO_EXPORT(PopGUIController, PartsEngine_PopGUIController),
	    HLL_EXPORT(SetPartsMagX, PE_SetPartsMagX),
	    HLL_EXPORT(SetPartsMagY, PE_SetPartsMagY),
	    HLL_EXPORT(SetPartsRotateX, PE_SetPartsRotateX),
	    HLL_EXPORT(SetPartsRotateY, PE_SetPartsRotateY),
	    HLL_EXPORT(SetPartsRotateZ, PE_SetPartsRotateZ),
	    HLL_EXPORT(SetPartsAlphaClipperPartsNumber, PE_SetPartsAlphaClipperPartsNumber),
	    HLL_EXPORT(SetPartsPixelDecide, PE_SetPartsPixelDecide),
	    HLL_EXPORT(IsCursorIn, PE_IsCursorIn),
	    HLL_EXPORT(SetThumbnailReductionSize, PE_SetThumbnailReductionSize),
	    HLL_EXPORT(SetThumbnailMode, PE_SetThumbnailMode),
	    HLL_EXPORT(Save, PE_Save),
	    HLL_EXPORT(SaveWithoutHideParts, PE_SaveWithoutHideParts),
	    HLL_EXPORT(Load, PE_Load),
	    // Rance 9
	    HLL_EXPORT(PartsFunc, PartsEngine_PartsFunc),
	    HLL_EXPORT(AddPartsConstructionProcess, PE_AddPartsConstructionProcess),
	    HLL_EXPORT(Release, PE_ReleaseParts),
	    HLL_TODO_EXPORT(ReleaseAll, PartsEngine_ReleaseAll),
	    HLL_EXPORT(ReleaseAllWithoutSystem, PE_ReleaseAllWithoutSystem),
	    HLL_EXPORT(GetFreeNumber, PE_GetFreeNumber),
	    HLL_EXPORT(IsExist, PE_IsExist),
	    HLL_EXPORT(AddController, PE_AddController),
	    HLL_EXPORT(SetActiveController, PE_set_active_controller),
	    HLL_EXPORT(GetActiveController, PE_get_active_controller),
	    HLL_EXPORT(GetControllerLength, PE_get_controller_length),
	    HLL_EXPORT(GetControllerID, PE_get_controller_id),
	    HLL_EXPORT(CreateActivity, PE_CreateActivity),
	    HLL_EXPORT(IsExistActivity, PE_IsExistActivity),
	    HLL_EXPORT(ReleaseActivity, PE_ReleaseActivity),
	    HLL_EXPORT(AddActivityParts, PE_AddActivityParts),
	    HLL_EXPORT(RemoveActivityParts, PE_RemoveActivityParts),
	    HLL_EXPORT(RemoveAllActivityParts, PE_RemoveAllActivityParts),
	    HLL_EXPORT(NumofActivityParts, PE_NumofActivityParts),
	    HLL_EXPORT(GetActivityParts, PE_GetActivityParts),
	    HLL_EXPORT(IsExistActivityPartsByName, PE_IsExistActivityPartsByName),
	    HLL_EXPORT(IsExistActivityPartsByNumber, PE_IsExistActivityPartsByNumber),
	    HLL_EXPORT(GetActivityPartsNumber, PE_GetActivityPartsNumber),
	    HLL_EXPORT(GetActivityPartsName, PE_GetActivityPartsName),
	    HLL_EXPORT(SetActivityEXText, PE_SetActivityEXText),
	    HLL_EXPORT(GetActivityEXText, PE_GetActivityEXText),
	    HLL_EXPORT(GetActivityEXID, PE_GetActivityEXID),
	    HLL_EXPORT(IsExistActivityFile, PE_IsExistActivityFile),
	    HLL_EXPORT(ReadActivityFile, PE_ReadActivityFile),
	    HLL_EXPORT(SetActivityEndKey, PE_SetActivityEndKey),
	    HLL_EXPORT(EraseActivityEndKey, PE_EraseActivityEndKey),
	    HLL_EXPORT(IsExistActivityEndKey, PE_IsExistActivityEndKey),
	    HLL_EXPORT(NumofActivityEndKey, PE_NumofActivityEndKey),
	    HLL_EXPORT(GetComponentAbsoluteMaxPosZ, PE_GetComponentAbsoluteMaxPosZ),
	    HLL_EXPORT(UpdateMatrix, PE_UpdateMatrix),
	    HLL_EXPORT(GetActivityEndKey, PE_GetActivityEndKey),
	    HLL_EXPORT(AddActivityCloseParts, PE_AddActivityCloseParts),
	    HLL_EXPORT(RemoveActivityCloseParts, PE_RemoveActivityCloseParts),
	    HLL_EXPORT(RemoveAllActivityCloseParts, PE_RemoveAllActivityCloseParts),
	    HLL_EXPORT(IsExistActivityCloseParts, PE_IsExistActivityCloseParts),
	    HLL_EXPORT(SetActivityIntentData, PE_SetActivityIntentData),
	    HLL_EXPORT(AddActivityIntentDataDestination, PE_AddActivityIntentDataDestination),
	    HLL_EXPORT(IsExistActivityIntentData, PE_IsExistActivityIntentData),
	    HLL_EXPORT(NumofActivityIntentDataDestination, PE_NumofActivityIntentDataDestination),
	    HLL_EXPORT(GetActivityIntentDataDestination, PE_GetActivityIntentDataDestination),
	    HLL_EXPORT(GetActivityIntentDataType, PE_GetActivityIntentDataType),
	    HLL_EXPORT(WriteActivityFile, PartsEngine_WriteActivityFile),
	    HLL_EXPORT(SaveActivityEXText, PartsEngine_SaveActivityEXText),
	    HLL_EXPORT(LoadActivityEXText, PartsEngine_LoadActivityEXText),
	    HLL_EXPORT(SetButtonSize, PartsEngine_SetButtonSize),
	    HLL_EXPORT(SetButtonEnable, PartsEngine_SetButtonEnable),
	    HLL_EXPORT(IsButtonEnable, PartsEngine_IsButtonEnable),
	    HLL_EXPORT(SetButtonColor, PartsEngine_SetButtonColor),
	    HLL_EXPORT(GetButtonR, PartsEngine_GetButtonR),
	    HLL_EXPORT(GetButtonG, PartsEngine_GetButtonG),
	    HLL_EXPORT(GetButtonB, PartsEngine_GetButtonB),
	    HLL_EXPORT(SetButtonFontProperty, PartsEngine_SetButtonFontProperty),
	    HLL_EXPORT(GetButtonFontProperty, PE_GetButtonFontProperty),
	    HLL_EXPORT(SetButtonCGName, PE_SetButtonCGName),
	    HLL_EXPORT(GetButtonCGName, PE_GetButtonCGName),
	    HLL_EXPORT(SetButtonFlatName, PE_SetButtonFlatName),
	    HLL_EXPORT(GetButtonFlatName, PE_GetButtonFlatName),
	    HLL_EXPORT(SetButtonText, PartsEngine_SetButtonText),
	    HLL_EXPORT(GetButtonText, PE_GetButtonText),
	    HLL_EXPORT(SetButtonTextOriginPosMode, PartsEngine_SetButtonTextOriginPosMode),
	    HLL_EXPORT(GetButtonTextOriginPosMode, PartsEngine_GetButtonTextOriginPosMode),
	    HLL_EXPORT(SetButtonCharSpace, PartsEngine_SetButtonCharSpace),
	    HLL_EXPORT(GetButtonCharSpace, PartsEngine_GetButtonCharSpace),
	    HLL_EXPORT(SetButtonLineSpace, PartsEngine_SetButtonLineSpace),
	    HLL_EXPORT(GetButtonLineSpace, PartsEngine_GetButtonLineSpace),
	    HLL_EXPORT(GetCheckBoxButtonWidth, PartsEngine_GetCheckBoxButtonWidth),
	    HLL_EXPORT(GetCheckBoxButtonHeight, PartsEngine_GetCheckBoxButtonHeight),
	    HLL_EXPORT(SetCheckBoxButtonMode, PartsEngine_SetCheckBoxButtonMode),
	    HLL_EXPORT(IsCheckBoxButtonMode, PartsEngine_IsCheckBoxButtonMode),
	    HLL_EXPORT(SetVScrollbarMoveSizeByButton, PartsEngine_SetVScrollbarMoveSizeByButton),
	    HLL_EXPORT(GetVScrollbarMoveSizeByButton, PartsEngine_GetVScrollbarMoveSizeByButton),
	    HLL_EXPORT(SetHScrollbarMoveSizeByButton, PartsEngine_SetHScrollbarMoveSizeByButton),
	    HLL_EXPORT(GetHScrollbarMoveSizeByButton, PartsEngine_GetHScrollbarMoveSizeByButton),
	    HLL_EXPORT(IsRadioButtonBoxExistGUI, PartsEngine_IsRadioButtonBoxExistGUI),
	    HLL_EXPORT(ClearRadioButtonBoxChild, PartsEngine_ClearRadioButtonBoxChild),
	    HLL_EXPORT(AddRadioButtonBoxChild, PartsEngine_AddRadioButtonBoxChild),
	    HLL_EXPORT(RemoveRadioButtonBoxChild, PartsEngine_RemoveRadioButtonBoxChild),
	    HLL_EXPORT(NumofRadioButtonBoxChild, PartsEngine_NumofRadioButtonBoxChild),
	    HLL_EXPORT(GetRadioButtonBoxChild, PartsEngine_GetRadioButtonBoxChild),
	    HLL_EXPORT(AddChild, PE_AddChild),
	    HLL_EXPORT(InsertChild, PE_InsertChild),
	    HLL_EXPORT(RemoveChild, PE_RemoveChild),
	    HLL_EXPORT(IsExistChild, PE_IsExistChild),
	    HLL_EXPORT(ClearChild, PartsEngine_ClearChild),
	    HLL_EXPORT(NumofChild, PartsEngine_NumofChild),
	    HLL_EXPORT(GetChild, PartsEngine_GetChild),
	    HLL_EXPORT(GetChildIndex, PartsEngine_GetChildIndex),
	    HLL_EXPORT(SetEventID, PartsEngine_SetEventID),
	    HLL_EXPORT(Parts_StopSwipe, PartsEngine_Parts_StopSwipe),
	    HLL_EXPORT(Parts_SetComment, PartsEngine_Parts_SetComment),
	    HLL_EXPORT(RemoveController, PE_RemoveController),
	    HLL_EXPORT(UpdateComponent, PartsEngine_Update),
	    HLL_EXPORT(Parts_SetThumbnailReductionSize, PE_SetThumbnailReductionSize),
	    HLL_EXPORT(Parts_SetThumbnailMode, PE_SetThumbnailMode),
	    HLL_EXPORT(GetClickNumber, PE_GetClickPartsNumber),
	    HLL_EXPORT(StopSoundWithoutSystemSound, PartsEngine_StopSoundWithoutSystemSound),
	    HLL_EXPORT(Parts_SetSoundName, PE_Parts_SetSoundName),
	    HLL_EXPORT(Parts_GetSoundName, PE_Parts_GetSoundName),
	    HLL_TODO_EXPORT(ReleaseActivity, PartsEngine_ReleaseActivity),
	    HLL_TODO_EXPORT(CrateActivityBinary, PartsEngine_CrateActivityBinary),
	    HLL_TODO_EXPORT(ReadActivityBinary, PartsEngine_ReadActivityBinary),
	    HLL_EXPORT(ReleaseMessage, PE_ReleaseMessage),
	    HLL_EXPORT(PopMessage, PE_PopMessage),
	    HLL_EXPORT(GetMessagePartsNumber, PE_GetMessagePartsNumber),
	    HLL_EXPORT(GetMessageDelegateIndex, PE_GetMessageDelegateIndex),
	    HLL_EXPORT(GetDelegateIndex, PE_GetDelegateIndex),
	    HLL_EXPORT(GetMessageType, PE_GetMessageType),
	    HLL_EXPORT(GetMessageVariableCount, PE_GetMessageVariableCount),
	    HLL_EXPORT(GetMessageVariableType, PE_GetMessageVariableType),
	    HLL_EXPORT(GetMessageVariableInt, PE_GetMessageVariableInt),
	    HLL_EXPORT(GetMessageVariableFloat, PE_GetMessageVariableFloat),
	    HLL_EXPORT(GetMessageVariableBool, PE_GetMessageVariableBool),
	    HLL_EXPORT(GetMessageVariableString, PE_GetMessageVariableString),
	    HLL_EXPORT(SetDelegateIndex, PE_SetDelegateIndex),
	    HLL_EXPORT(SetFocus, PartsEngine_SetFocus),
	    HLL_EXPORT(IsFocus, PE_IsFocus),
	    HLL_EXPORT(SetComponentType, PE_SetComponentType),
	    HLL_EXPORT(GetComponentType, PE_GetComponentType),
	    HLL_EXPORT(SetComponentPos, PartsEngine_SetComponentPos),
	    HLL_EXPORT(SetComponentPosZ, PE_SetZ),
	    HLL_EXPORT(GetComponentPosX, PartsEngine_Parts_GetComponentPosX),
	    HLL_EXPORT(GetComponentPosY, PartsEngine_GetComponentPosY),
	    HLL_EXPORT(GetComponentPosZ, PE_GetPartsZ),
	    HLL_EXPORT(Parts_GetPartsUpperLeftPosX, PartsEngine_Parts_GetPartsUpperLeftPosX),
	    HLL_EXPORT(Parts_GetPartsUpperLeftPosY, PartsEngine_Parts_GetPartsUpperLeftPosY),
	    HLL_EXPORT(SetComponentOriginPosMode, PE_SetPartsOriginPosMode),
	    HLL_EXPORT(GetComponentOriginPosMode, PE_GetPartsOriginPosMode),
	    HLL_TODO_EXPORT(GetComponentWidth, PartsEngine_GetComponentWidth),
	    HLL_TODO_EXPORT(GetComponentHeight, PartsEngine_GetComponentHeight),
	    HLL_EXPORT(Parts_GetPartsWidth, PE_GetPartsWidth),
	    HLL_EXPORT(Parts_GetPartsHeight, PE_GetPartsHeight),
	    HLL_EXPORT(SetComponentShow, PE_SetShow),
	    HLL_EXPORT(IsComponentShow, PE_GetPartsShow),
	    HLL_EXPORT(SetComponentMessageWindowShowLink, PE_SetPartsMessageWindowShowLink),
	    HLL_EXPORT(IsComponentMessageWindowShowLink, PE_GetPartsMessageWindowShowLink),
	    HLL_EXPORT(SetComponentAlpha, PE_SetAlpha),
	    HLL_EXPORT(GetComponentAlpha, PE_GetPartsAlpha),
	    HLL_EXPORT(SetComponentAddColor, PE_SetAddColor),
	    HLL_TODO_EXPORT(GetComponentAddColorR, PartsEngine_GetComponentAddColorR),
	    HLL_TODO_EXPORT(GetComponentAddColorG, PartsEngine_GetComponentAddColorG),
	    HLL_TODO_EXPORT(GetComponentAddColorB, PartsEngine_GetComponentAddColorB),
	    HLL_EXPORT(SetComponentMulColor, PE_SetMultiplyColor),
	    HLL_TODO_EXPORT(GetComponentMulColorR, PartsEngine_GetComponentMulColorR),
	    HLL_TODO_EXPORT(GetComponentMulColorG, PartsEngine_GetComponentMulColorG),
	    HLL_TODO_EXPORT(GetComponentMulColorB, PartsEngine_GetComponentMulColorB),
	    HLL_EXPORT(SetComponentDrawFilter, PE_SetPartsDrawFilter),
	    HLL_TODO_EXPORT(GetComponentDrawFilter, PartsEngine_GetComponentDrawFilter),
	    HLL_EXPORT(SetComponentMagX, PE_SetPartsMagX),
	    HLL_EXPORT(SetComponentMagY, PE_SetPartsMagY),
	    HLL_TODO_EXPORT(GetComponentMagX, PartsEngine_GetComponentMagX),
	    HLL_TODO_EXPORT(GetComponentMagY, PartsEngine_GetComponentMagY),
	    HLL_EXPORT(SetComponentRotateX, PE_SetPartsRotateX),
	    HLL_EXPORT(SetComponentRotateY, PE_SetPartsRotateY),
	    HLL_EXPORT(SetComponentRotateZ, PE_SetPartsRotateZ),
	    HLL_TODO_EXPORT(GetComponentRotateX, PartsEngine_GetComponentRotateX),
	    HLL_TODO_EXPORT(GetComponentRotateY, PartsEngine_GetComponentRotateY),
	    HLL_EXPORT(GetComponentRotateZ, PE_GetPartsRotateZ),
	    HLL_EXPORT(SetComponentMargin, PE_SetComponentMargin),
	    HLL_EXPORT(GetComponentMarginTop, PE_GetComponentMarginTop),
	    HLL_EXPORT(GetComponentMarginBottom, PE_GetComponentMarginBottom),
	    HLL_EXPORT(GetComponentMarginLeft, PE_GetComponentMarginLeft),
	    HLL_EXPORT(GetComponentMarginRight, PE_GetComponentMarginRight),
	    HLL_EXPORT(SetComponentAlphaClipper, PE_SetPartsAlphaClipperPartsNumber),
	    HLL_TODO_EXPORT(GetComponentAlphaClipper, PartsEngine_GetComponentAlphaClipper),
	    HLL_TODO_EXPORT(SetComponentTextureFilterType, PartsEngine_SetComponentTextureFilterType),
	    HLL_TODO_EXPORT(GetComponentTextureFilterType, PartsEngine_GetComponentTextureFilterType),
	    HLL_TODO_EXPORT(SetComponentMipmap, PartsEngine_SetComponentMipmap),
	    HLL_TODO_EXPORT(IsComponentMipmap, PartsEngine_IsComponentMipmap),
	    HLL_EXPORT(SetComponentSpeedupRateByMessageSkip, PE_SetSpeedupRateByMessageSkip),
	    HLL_TODO_EXPORT(GetComponentSpeedupRateByMessageSkip, PartsEngine_GetComponentSpeedupRateByMessageSkip),
	    HLL_EXPORT(AddComponentMotionPos, PartsEngine_AddComponentMotionPos),
	    HLL_EXPORT(AddComponentMotionAlpha, PE_AddMotionAlpha_curve),
	    HLL_TODO_EXPORT(AddComponentMotionCG, PartsEngine_AddComponentMotionCG),
	    HLL_TODO_EXPORT(AddComponentMotionCGTermination, PartsEngine_AddComponentMotionCGTermination),
	    HLL_EXPORT(AddComponentMotionHGaugeRate, PE_AddMotionHGaugeRate_curve),
	    HLL_EXPORT(AddComponentMotionVGaugeRate, PE_AddMotionVGaugeRate_curve),
	    HLL_EXPORT(AddComponentMotionNumeralNumber, PE_AddMotionNumeralNumber_curve),
	    HLL_EXPORT(AddComponentMotionMagX, PE_AddMotionMagX_curve),
	    HLL_EXPORT(AddComponentMotionMagY, PE_AddMotionMagY_curve),
	    HLL_EXPORT(AddComponentMotionRotateX, PE_AddMotionRotateX_curve),
	    HLL_EXPORT(AddComponentMotionRotateY, PE_AddMotionRotateY_curve),
	    HLL_EXPORT(AddComponentMotionRotateZ, PE_AddMotionRotateZ_curve),
	    HLL_EXPORT(AddComponentMotionVibrationSize, PE_AddMotionVibrationSize),
	    HLL_TODO_EXPORT(SuspendBuildView, PartsEngine_SuspendBuildView),
	    HLL_TODO_EXPORT(SuspendBuildViewAt, PartsEngine_SuspendBuildViewAt),
	    HLL_TODO_EXPORT(ResumeBuildView, PartsEngine_ResumeBuildView),
	    HLL_TODO_EXPORT(SetButtonSize, PartsEngine_SetButtonSize),
	    HLL_TODO_EXPORT(SetButtonDrag, PartsEngine_SetButtonDrag),
	    HLL_TODO_EXPORT(IsButtonDrag, PartsEngine_IsButtonDrag),
	    HLL_TODO_EXPORT(SetButtonEnable, PartsEngine_SetButtonEnable),
	    HLL_TODO_EXPORT(IsButtonEnable, PartsEngine_IsButtonEnable),
	    HLL_TODO_EXPORT(SetButtonPixelDecide, PartsEngine_SetButtonPixelDecide),
	    HLL_TODO_EXPORT(IsButtonPixelDecide, PartsEngine_IsButtonPixelDecide),
	    HLL_TODO_EXPORT(SetButtonColor, PartsEngine_SetButtonColor),
	    HLL_TODO_EXPORT(GetButtonR, PartsEngine_GetButtonR),
	    HLL_TODO_EXPORT(GetButtonG, PartsEngine_GetButtonG),
	    HLL_TODO_EXPORT(GetButtonB, PartsEngine_GetButtonB),
	    HLL_TODO_EXPORT(SetButtonFontProperty, PartsEngine_SetButtonFontProperty),
	    HLL_TODO_EXPORT(GetButtonFontProperty, PartsEngine_GetButtonFontProperty),
	    HLL_TODO_EXPORT(SetButtonOnCursorSoundNumber, PartsEngine_SetButtonOnCursorSoundNumber),
	    HLL_TODO_EXPORT(SetButtonClickSoundNumber, PartsEngine_SetButtonClickSoundNumber),
	    HLL_TODO_EXPORT(GetButtonOnCursorSoundNumber, PartsEngine_GetButtonOnCursorSoundNumber),
	    HLL_TODO_EXPORT(GetButtonClickSoundNumber, PartsEngine_GetButtonClickSoundNumber),
	    HLL_TODO_EXPORT(SetButtonCGName, PartsEngine_SetButtonCGName),
	    HLL_TODO_EXPORT(GetButtonCGName, PartsEngine_GetButtonCGName),
	    HLL_TODO_EXPORT(SetButtonText, PartsEngine_SetButtonText),
	    HLL_TODO_EXPORT(GetButtonText, PartsEngine_GetButtonText),
	    HLL_TODO_EXPORT(SetCheckBoxSize, PartsEngine_SetCheckBoxSize),
	    HLL_TODO_EXPORT(SetCheckBoxDrag, PartsEngine_SetCheckBoxDrag),
	    HLL_TODO_EXPORT(IsCheckBoxDrag, PartsEngine_IsCheckBoxDrag),
	    HLL_EXPORT(CheckBoxChecked, PE_SetPartsCheckBoxChecked),
	    HLL_EXPORT(IsCheckBoxChecked, PE_GetPartsCheckBoxChecked),
	    HLL_EXPORT(SetCheckBoxColor, PE_SetPartsCheckBoxColor),
	    HLL_EXPORT(GetCheckBoxR, PE_GetPartsCheckBoxR),
	    HLL_EXPORT(GetCheckBoxG, PE_GetPartsCheckBoxG),
	    HLL_EXPORT(GetCheckBoxB, PE_GetPartsCheckBoxB),
	    HLL_TODO_EXPORT(SetCheckBoxFontProperty, PartsEngine_SetCheckBoxFontProperty),
	    HLL_TODO_EXPORT(GetCheckBoxFontProperty, PartsEngine_GetCheckBoxFontProperty),
	    HLL_TODO_EXPORT(SetCheckBoxOnCursorSoundNumber, PartsEngine_SetCheckBoxOnCursorSoundNumber),
	    HLL_TODO_EXPORT(SetCheckBoxClickSoundNumber, PartsEngine_SetCheckBoxClickSoundNumber),
	    HLL_TODO_EXPORT(GetCheckBoxOnCursorSoundNumber, PartsEngine_GetCheckBoxOnCursorSoundNumber),
	    HLL_TODO_EXPORT(GetCheckBoxClickSoundNumber, PartsEngine_GetCheckBoxClickSoundNumber),
	    HLL_TODO_EXPORT(SetCheckBoxCGName, PartsEngine_SetCheckBoxCGName),
	    HLL_TODO_EXPORT(GetCheckBoxCGName, PartsEngine_GetCheckBoxCGName),
	    HLL_TODO_EXPORT(SetCheckBoxText, PartsEngine_SetCheckBoxText),
	    HLL_TODO_EXPORT(GetCheckBoxText, PartsEngine_GetCheckBoxText),
	    HLL_EXPORT(SetVScrollbarOnCursorSoundNumber, PartsEngine_SetVScrollbarOnCursorSoundNumber),
	    HLL_EXPORT(SetVScrollbarClickSoundNumber, PartsEngine_SetVScrollbarClickSoundNumber),
	    HLL_EXPORT(GetVScrollbarOnCursorSoundNumber, PartsEngine_GetVScrollbarOnCursorSoundNumber),
	    HLL_EXPORT(GetVScrollbarClickSoundNumber, PartsEngine_GetVScrollbarClickSoundNumber),
	    HLL_EXPORT(SetVScrollbarSize, PartsEngine_SetVScrollbarSize),
	    HLL_EXPORT(SetVScrollbarUpHeight, PartsEngine_SetVScrollbarUpHeight),
	    HLL_EXPORT(SetVScrollbarDownHeight, PartsEngine_SetVScrollbarDownHeight),
	    HLL_EXPORT(GetVScrollbarUpHeight, PartsEngine_GetVScrollbarUpHeight),
	    HLL_EXPORT(GetVScrollbarDownHeight, PartsEngine_GetVScrollbarDownHeight),
	    HLL_EXPORT(SetVScrollbarTotalSize, PartsEngine_SetVScrollbarTotalSize),
	    HLL_EXPORT(SetVScrollbarViewSize, PartsEngine_SetVScrollbarViewSize),
	    HLL_EXPORT(SetVScrollbarScrollPos, PartsEngine_SetVScrollbarScrollPos),
	    HLL_EXPORT(SetVScrollbarScrollRate, PartsEngine_SetVScrollbarScrollRate),
	    HLL_EXPORT(GetVScrollbarTotalSize, PartsEngine_GetVScrollbarTotalSize),
	    HLL_EXPORT(GetVScrollbarViewSize, PartsEngine_GetVScrollbarViewSize),
	    HLL_EXPORT(GetVScrollbarScrollPos, PartsEngine_GetVScrollbarScrollPos),
	    HLL_EXPORT(GetVScrollbarScrollRate, PartsEngine_GetVScrollbarScrollRate),
	    HLL_EXPORT(SetVScrollbarCGName, PartsEngine_SetVScrollbarCGName),
	    HLL_EXPORT(GetVScrollbarCGName, PartsEngine_GetVScrollbarCGName),
	    HLL_EXPORT(SetVScrollbarFlatName, PartsEngine_SetVScrollbarFlatName),
	    HLL_EXPORT(GetVScrollbarFlatName, PartsEngine_GetVScrollbarFlatName),
	    HLL_EXPORT(SetHScrollbarOnCursorSoundNumber, PartsEngine_SetHScrollbarOnCursorSoundNumber),
	    HLL_EXPORT(SetHScrollbarClickSoundNumber, PartsEngine_SetHScrollbarClickSoundNumber),
	    HLL_EXPORT(GetHScrollbarOnCursorSoundNumber, PartsEngine_GetHScrollbarOnCursorSoundNumber),
	    HLL_EXPORT(GetHScrollbarClickSoundNumber, PartsEngine_GetHScrollbarClickSoundNumber),
	    HLL_EXPORT(SetHScrollbarSize, PartsEngine_SetHScrollbarSize),
	    HLL_EXPORT(SetHScrollbarLeftWidth, PartsEngine_SetHScrollbarLeftWidth),
	    HLL_EXPORT(SetHScrollbarRightWidth, PartsEngine_SetHScrollbarRightWidth),
	    HLL_EXPORT(GetHScrollbarLeftWidth, PartsEngine_GetHScrollbarLeftWidth),
	    HLL_EXPORT(GetHScrollbarRightWidth, PartsEngine_GetHScrollbarRightWidth),
	    HLL_EXPORT(SetHScrollbarTotalSize, PartsEngine_SetHScrollbarTotalSize),
	    HLL_EXPORT(SetHScrollbarViewSize, PartsEngine_SetHScrollbarViewSize),
	    HLL_EXPORT(SetHScrollbarScrollPos, PartsEngine_SetHScrollbarScrollPos),
	    HLL_EXPORT(SetHScrollbarScrollRate, PE_SetPartsHScrollbarScrollRate),
	    HLL_EXPORT(GetHScrollbarTotalSize, PartsEngine_GetHScrollbarTotalSize),
	    HLL_EXPORT(GetHScrollbarViewSize, PartsEngine_GetHScrollbarViewSize),
	    HLL_EXPORT(GetHScrollbarScrollPos, PartsEngine_GetHScrollbarScrollPos),
	    HLL_EXPORT(GetHScrollbarScrollRate, PE_GetPartsHScrollbarScrollRate),
	    HLL_EXPORT(SetHScrollbarCGName, PartsEngine_SetHScrollbarCGName),
	    HLL_EXPORT(GetHScrollbarCGName, PartsEngine_GetHScrollbarCGName),
	    HLL_EXPORT(SetHScrollbarFlatName, PartsEngine_SetHScrollbarFlatName),
	    HLL_EXPORT(GetHScrollbarFlatName, PartsEngine_GetHScrollbarFlatName),
	    HLL_TODO_EXPORT(SetTextBoxSize, PartsEngine_SetTextBoxSize),
	    HLL_TODO_EXPORT(SetTextBoxFontProperty, PartsEngine_SetTextBoxFontProperty),
	    HLL_TODO_EXPORT(GetTextBoxFontProperty, PartsEngine_GetTextBoxFontProperty),
	    HLL_TODO_EXPORT(SetTextBoxText, PartsEngine_SetTextBoxText),
	    HLL_TODO_EXPORT(GetTextBoxText, PartsEngine_GetTextBoxText),
	    HLL_TODO_EXPORT(SetTextBoxMaxTextLength, PartsEngine_SetTextBoxMaxTextLength),
	    HLL_TODO_EXPORT(GetTextBoxMaxTextLength, PartsEngine_GetTextBoxMaxTextLength),
	    HLL_TODO_EXPORT(SetTextBoxSelectColor, PartsEngine_SetTextBoxSelectColor),
	    HLL_TODO_EXPORT(GetTextBoxSelectR, PartsEngine_GetTextBoxSelectR),
	    HLL_TODO_EXPORT(GetTextBoxSelectG, PartsEngine_GetTextBoxSelectG),
	    HLL_TODO_EXPORT(GetTextBoxSelectB, PartsEngine_GetTextBoxSelectB),
	    HLL_TODO_EXPORT(SetTextBoxCGName, PartsEngine_SetTextBoxCGName),
	    HLL_TODO_EXPORT(GetTextBoxCGName, PartsEngine_GetTextBoxCGName),
	    HLL_TODO_EXPORT(OpenTextBoxIME, PartsEngine_OpenTextBoxIME),
	    HLL_TODO_EXPORT(CloseTextBoxIME, PartsEngine_CloseTextBoxIME),
	    HLL_TODO_EXPORT(SetListBoxSize, PartsEngine_SetListBoxSize),
	    HLL_TODO_EXPORT(SetListBoxLineHeight, PartsEngine_SetListBoxLineHeight),
	    HLL_TODO_EXPORT(GetListBoxLineHeight, PartsEngine_GetListBoxLineHeight),
	    HLL_TODO_EXPORT(SetListBoxMargin, PartsEngine_SetListBoxMargin),
	    HLL_TODO_EXPORT(GetListBoxWidthMargin, PartsEngine_GetListBoxWidthMargin),
	    HLL_TODO_EXPORT(GetListBoxHeightMargin, PartsEngine_GetListBoxHeightMargin),
	    HLL_TODO_EXPORT(SetListBoxCGName, PartsEngine_SetListBoxCGName),
	    HLL_TODO_EXPORT(GetListBoxCGName, PartsEngine_GetListBoxCGName),
	    HLL_TODO_EXPORT(SetListBoxScrollPos, PartsEngine_SetListBoxScrollPos),
	    HLL_TODO_EXPORT(GetListBoxScrollPos, PartsEngine_GetListBoxScrollPos),
	    HLL_TODO_EXPORT(AddListBoxItem, PartsEngine_AddListBoxItem),
	    HLL_TODO_EXPORT(InsertListBoxItem, PartsEngine_InsertListBoxItem),
	    HLL_TODO_EXPORT(SetListBoxItem, PartsEngine_SetListBoxItem),
	    HLL_TODO_EXPORT(GetListBoxItemCount, PartsEngine_GetListBoxItemCount),
	    HLL_TODO_EXPORT(GetListBoxItem, PartsEngine_GetListBoxItem),
	    HLL_TODO_EXPORT(EraseListBoxItem, PartsEngine_EraseListBoxItem),
	    HLL_TODO_EXPORT(ClearListBoxItem, PartsEngine_ClearListBoxItem),
	    HLL_TODO_EXPORT(GetListBoxOnCursorItemIndex, PartsEngine_GetListBoxOnCursorItemIndex),
	    HLL_TODO_EXPORT(GetListBoxOnCursorItem, PartsEngine_GetListBoxOnCursorItem),
	    HLL_TODO_EXPORT(SetListBoxFontProperty, PartsEngine_SetListBoxFontProperty),
	    HLL_TODO_EXPORT(GetListBoxFontProperty, PartsEngine_GetListBoxFontProperty),
	    HLL_TODO_EXPORT(SetListBoxSelectIndex, PartsEngine_SetListBoxSelectIndex),
	    HLL_TODO_EXPORT(GetListBoxSelectIndex, PartsEngine_GetListBoxSelectIndex),
	    HLL_TODO_EXPORT(SetComboBoxSize, PartsEngine_SetComboBoxSize),
	    HLL_TODO_EXPORT(SetComboBoxTextMargin, PartsEngine_SetComboBoxTextMargin),
	    HLL_TODO_EXPORT(GetComboBoxTextWidthMargin, PartsEngine_GetComboBoxTextWidthMargin),
	    HLL_TODO_EXPORT(GetComboBoxTextHeightMargin, PartsEngine_GetComboBoxTextHeightMargin),
	    HLL_TODO_EXPORT(SetComboBoxCGName, PartsEngine_SetComboBoxCGName),
	    HLL_TODO_EXPORT(GetComboBoxCGName, PartsEngine_GetComboBoxCGName),
	    HLL_TODO_EXPORT(SetComboBoxText, PartsEngine_SetComboBoxText),
	    HLL_TODO_EXPORT(GetComboBoxText, PartsEngine_GetComboBoxText),
	    HLL_TODO_EXPORT(SetComboBoxFontProperty, PartsEngine_SetComboBoxFontProperty),
	    HLL_TODO_EXPORT(GetComboBoxFontProperty, PartsEngine_GetComboBoxFontProperty),
	    HLL_TODO_EXPORT(SetMultiTextBoxSize, PartsEngine_SetMultiTextBoxSize),
	    HLL_TODO_EXPORT(SetMultiTextBoxFontProperty, PartsEngine_SetMultiTextBoxFontProperty),
	    HLL_TODO_EXPORT(GetMultiTextBoxFontProperty, PartsEngine_GetMultiTextBoxFontProperty),
	    HLL_TODO_EXPORT(SetMultiTextBoxText, PartsEngine_SetMultiTextBoxText),
	    HLL_TODO_EXPORT(GetMultiTextBoxText, PartsEngine_GetMultiTextBoxText),
	    HLL_TODO_EXPORT(SetMultiTextBoxMaxTextLength, PartsEngine_SetMultiTextBoxMaxTextLength),
	    HLL_TODO_EXPORT(GetMultiTextBoxMaxTextLength, PartsEngine_GetMultiTextBoxMaxTextLength),
	    HLL_TODO_EXPORT(SetMultiTextBoxSelectColor, PartsEngine_SetMultiTextBoxSelectColor),
	    HLL_TODO_EXPORT(GetMultiTextBoxSelectR, PartsEngine_GetMultiTextBoxSelectR),
	    HLL_TODO_EXPORT(GetMultiTextBoxSelectG, PartsEngine_GetMultiTextBoxSelectG),
	    HLL_TODO_EXPORT(GetMultiTextBoxSelectB, PartsEngine_GetMultiTextBoxSelectB),
	    HLL_TODO_EXPORT(SetMultiTextBoxCGName, PartsEngine_SetMultiTextBoxCGName),
	    HLL_TODO_EXPORT(GetMultiTextBoxCGName, PartsEngine_GetMultiTextBoxCGName),
	    HLL_EXPORT(SetLayoutBoxLayoutType, PE_SetLayoutBoxLayoutType),
	    HLL_EXPORT(GetLayoutBoxLayoutType, PE_GetLayoutBoxLayoutType),
	    HLL_EXPORT(SetLayoutBoxReturn, PE_SetLayoutBoxReturn),
	    HLL_EXPORT(IsLayoutBoxReturn, PE_IsLayoutBoxReturn),
	    HLL_EXPORT(GetLayoutBoxReturnSize, PE_GetLayoutBoxReturnSize),
	    HLL_EXPORT(SetLayoutBoxAlign, PE_SetLayoutBoxAlign),
	    // Панель (Ixseal, component type 14) — src/parts/panel.c
	    HLL_EXPORT(SetPanelSize, PE_SetPanelSize),
	    HLL_EXPORT(SetPanelColor, PE_SetPanelColor),
	    HLL_EXPORT(GetPanelR, PE_GetPanelR),
	    HLL_EXPORT(GetPanelG, PE_GetPanelG),
	    HLL_EXPORT(GetPanelB, PE_GetPanelB),
	    HLL_EXPORT(GetPanelA, PE_GetPanelA),
	    HLL_EXPORT(SetPanelAlphaGradationTop, PE_SetPanelAlphaGradationTop),
	    HLL_EXPORT(SetPanelAlphaGradationBottom, PE_SetPanelAlphaGradationBottom),
	    HLL_EXPORT(SetPanelAlphaGradationLeft, PE_SetPanelAlphaGradationLeft),
	    HLL_EXPORT(SetPanelAlphaGradationRight, PE_SetPanelAlphaGradationRight),
	    HLL_EXPORT(GetPanelAlphaGradationTop, PE_GetPanelAlphaGradationTop),
	    HLL_EXPORT(GetPanelAlphaGradationBottom, PE_GetPanelAlphaGradationBottom),
	    HLL_EXPORT(GetPanelAlphaGradationLeft, PE_GetPanelAlphaGradationLeft),
	    HLL_EXPORT(GetPanelAlphaGradationRight, PE_GetPanelAlphaGradationRight),
	    HLL_EXPORT(GetLayoutBoxAlign, PE_GetLayoutBoxAlign),
	    HLL_EXPORT(Parts_SetPartsCG, PE_SetPartsCG),
	    HLL_EXPORT(Parts_GetPartsCGName, PE_GetPartsCGName),
	    HLL_EXPORT(Parts_SetPartsCGSurfaceArea, PE_SetPartsCGSurfaceArea),
	    HLL_EXPORT(Parts_SetLoopCG, PE_SetLoopCG),
	    HLL_EXPORT(Parts_SetLoopCGSurfaceArea, PE_SetLoopCGSurfaceArea),
	    HLL_EXPORT(Parts_SetText, PE_SetText),
	    HLL_EXPORT(Parts_AddPartsText, PE_AddPartsText),
	    HLL_TODO_EXPORT(Parts_DeletePartsTopTextLine, PartsEngine_Parts_DeletePartsTopTextLine),
	    HLL_EXPORT(Parts_SetPartsTextSurfaceArea, PE_SetPartsTextSurfaceArea),
	    HLL_TODO_EXPORT(Parts_SetPartsTextHighlight, PartsEngine_Parts_SetPartsTextHighlight),
	    HLL_TODO_EXPORT(Parts_AddPartsTextHighlight, PartsEngine_Parts_AddPartsTextHighlight),
	    HLL_TODO_EXPORT(Parts_ClearPartsTextHighlight, PartsEngine_Parts_ClearPartsTextHighlight),
	    HLL_TODO_EXPORT(Parts_SetPartsTextCountReturn, PartsEngine_Parts_SetPartsTextCountReturn),
	    HLL_TODO_EXPORT(Parts_GetPartsTextCountReturn, PartsEngine_Parts_GetPartsTextCountReturn),
	    HLL_EXPORT(Parts_SetFont, PE_SetFont),
	    HLL_EXPORT(Parts_SetPartsFontType, PE_SetPartsFontType),
	    HLL_EXPORT(Parts_SetPartsFontSize, PE_SetPartsFontSize),
	    HLL_EXPORT(Parts_SetPartsFontColor, PE_SetPartsFontColor),
	    HLL_EXPORT(Parts_SetPartsFontBoldWeight, PE_SetPartsFontBoldWeight),
	    HLL_EXPORT(Parts_SetPartsFontEdgeColor, PE_SetPartsFontEdgeColor),
	    HLL_EXPORT(Parts_SetPartsFontEdgeWeight, PE_SetPartsFontEdgeWeight),
	    HLL_EXPORT(Parts_SetTextCharSpace, PE_SetTextCharSpace),
	    HLL_EXPORT(Parts_SetTextLineSpace, PE_SetTextLineSpace),
	    HLL_EXPORT(Parts_GetTextCharSpace, PE_GetTextCharSpace),
	    HLL_EXPORT(Parts_GetTextLineSpace, PE_GetTextLineSpace),
	    HLL_EXPORT(Parts_SetHGaugeCG, PE_SetHGaugeCG),
	    HLL_EXPORT(Parts_SetHGaugeRate, PE_SetHGaugeRate),
	    HLL_EXPORT(Parts_SetVGaugeCG, PE_SetVGaugeCG),
	    HLL_EXPORT(Parts_SetVGaugeRate, PE_SetVGaugeRate),
	    HLL_EXPORT(Parts_SetHGaugeSurfaceArea, PE_SetHGaugeSurfaceArea),
	    HLL_EXPORT(Parts_SetVGaugeSurfaceArea, PE_SetVGaugeSurfaceArea),
	    HLL_EXPORT(Parts_SetNumeralCG, PE_SetNumeralCG),
	    HLL_EXPORT(Parts_SetNumeralLinkedCGNumberWidthWidthList, PE_SetNumeralLinkedCGNumberWidthWidthList),
	    HLL_TODO_EXPORT(Parts_SetNumeralFont, PartsEngine_Parts_SetNumeralFont),
	    HLL_EXPORT(Parts_SetNumeralNumber, PE_SetNumeralNumber),
	    HLL_EXPORT(Parts_SetNumeralShowComma, PE_SetNumeralShowComma),
	    HLL_EXPORT(Parts_SetNumeralSpace, PE_SetNumeralSpace),
	    HLL_EXPORT(Parts_SetNumeralLength, PE_SetNumeralLength),
	    HLL_EXPORT(Parts_SetNumeralSurfaceArea, PE_SetNumeralSurfaceArea),
	    HLL_EXPORT(Parts_SetPartsRectangleDetectionSize, PE_SetPartsRectangleDetectionSize),
	    HLL_TODO_EXPORT(Parts_SetPartsRectangleDetectionSurfaceArea, PartsEngine_Parts_SetPartsRectangleDetectionSurfaceArea),
	    HLL_EXPORT(Parts_SetPartsCGDetectionSize, PE_SetPartsCGDetectionSize),
	    HLL_TODO_EXPORT(Parts_SetPartsCGDetectionSurfaceArea, PartsEngine_Parts_SetPartsCGDetectionSurfaceArea),
	    HLL_EXPORT(Parts_SetPartsFlat, PE_SetPartsFlat),
	    HLL_EXPORT(Parts_IsPartsFlatEnd, PE_IsPartsFlatEnd),
	    HLL_EXPORT(Parts_GetPartsFlatCurrentFrameNumber, PE_GetPartsFlatCurrentFrameNumber),
	    HLL_EXPORT(Parts_BackPartsFlatBeginFrame, PE_BackPartsFlatBeginFrame),
	    HLL_EXPORT(Parts_StepPartsFlatFinalFrame, PE_StepPartsFlatFinalFrame),
	    HLL_EXPORT(Parts_SetPartsFlatSurfaceArea, PE_SetPartsFlatSurfaceArea),
	    HLL_EXPORT(Parts_SetPartsFlatAndStop, PE_SetPartsFlatAndStop),
	    HLL_EXPORT(Parts_StopPartsFlat, PE_StopPartsFlat),
	    HLL_EXPORT(Parts_StartPartsFlat, PE_StartPartsFlat),
	    HLL_EXPORT(Parts_GoFramePartsFlat, PE_GoFramePartsFlat),
	    HLL_EXPORT(Parts_GetPartsFlatEndFrame, PE_GetPartsFlatEndFrame),
	    HLL_EXPORT(Parts_ExistsFlatFile, PE_ExistsFlatFile),
	    HLL_TODO_EXPORT(Parts_GetPartsFlatDataInfo, PartsEngine_Parts_GetPartsFlatDataInfo),
	    HLL_TODO_EXPORT(Parts_ChangeFlatCG, PartsEngine_Parts_ChangeFlatCG),
	    HLL_TODO_EXPORT(Parts_ChangeFlatSound, PartsEngine_Parts_ChangeFlatSound),
	    HLL_EXPORT(Parts_ClearPartsConstructionProcess, PE_ClearPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddCreateToPartsConstructionProcess, PE_AddCreateToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddCreatePixelOnlyToPartsConstructionProcess, PE_AddCreatePixelOnlyToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddCreateCGToProcess, PE_AddCreateCGToProcess),
	    HLL_EXPORT(Parts_AddFillToPartsConstructionProcess, PE_AddFillToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddFillAlphaColorToPartsConstructionProcess, PE_AddFillAlphaColorToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddFillAMapToPartsConstructionProcess, PE_AddFillAMapToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddFillWithAlphaToPartsConstructionProcess, PE_AddFillWithAlphaToPartsConstructionProcess),
	    HLL_TODO_EXPORT(Parts_AddFillGradationHorizonToPartsConstructionProcess, PartsEngine_Parts_AddFillGradationHorizonToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddDrawRectToPartsConstructionProcess, PE_AddDrawRectToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddDrawCutCGToPartsConstructionProcess, PE_AddDrawCutCGToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddCopyCutCGToPartsConstructionProcess, PE_AddCopyCutCGToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddGrayFilterToPartsConstructionProcess, PE_AddGrayFilterToPartsConstructionProcess),
	    HLL_TODO_EXPORT(Parts_AddAddFilterToPartsConstructionProcess, PartsEngine_Parts_AddAddFilterToPartsConstructionProcess),
	    HLL_TODO_EXPORT(Parts_AddMulFilterToPartsConstructionProcess, PartsEngine_Parts_AddMulFilterToPartsConstructionProcess),
	    HLL_TODO_EXPORT(Parts_AddDrawLineToPartsConstructionProcess, PartsEngine_Parts_AddDrawLineToPartsConstructionProcess),
	    HLL_EXPORT(Parts_BuildPartsConstructionProcess, PE_BuildPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddDrawTextToPartsConstructionProcess, PE_AddDrawTextToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddCopyTextToPartsConstructionProcess, PE_AddCopyTextToPartsConstructionProcess),
	    HLL_EXPORT(Parts_SetPartsConstructionSurfaceArea, PE_SetPartsConstructionSurfaceArea),
	    HLL_EXPORT(Parts_CreateParts3DLayerPluginID, PE_CreateParts3DLayerPluginID),
	    HLL_EXPORT(Parts_GetParts3DLayerPluginID, PE_GetParts3DLayerPluginID),
	    HLL_EXPORT(Parts_ReleaseParts3DLayerPluginID, PE_ReleaseParts3DLayerPluginID),
	    HLL_EXPORT(Parts_SetPassCursor, PE_SetPassCursor),
	    HLL_EXPORT(Parts_SetClickable, PE_SetClickable),
	    HLL_TODO_EXPORT(Parts_SetResetTimerByChangeInputStatus, PartsEngine_Parts_SetResetTimerByChangeInputStatus),
	    HLL_EXPORT(Parts_SetDrag, PE_SetDrag),
	    HLL_EXPORT(Parts_SetParentPartsNumber, PE_SetParentPartsNumber),
	    HLL_EXPORT(Parts_SetInputState, PE_SetInputState),
	    HLL_EXPORT(Parts_SetOnCursorShowLinkPartsNumber, PE_SetOnCursorShowLinkPartsNumber),
	    HLL_TODO_EXPORT(Parts_SetSoundNumber, PartsEngine_Parts_SetSoundNumber),
	    HLL_EXPORT(Parts_SetPartsPixelDecide, PE_SetPartsPixelDecide),
	    HLL_EXPORT(Parts_GetPartsPassCursor, PE_GetPartsPassCursor),
	    HLL_EXPORT(Parts_GetPartsClickable, PE_GetPartsClickable),
	    HLL_TODO_EXPORT(Parts_GetResetTimerByChangeInputStatus, PartsEngine_Parts_GetResetTimerByChangeInputStatus),
	    HLL_TODO_EXPORT(Parts_GetPartsDrag, PartsEngine_Parts_GetPartsDrag),
	    HLL_TODO_EXPORT(Parts_GetParentPartsNumber, PartsEngine_Parts_GetParentPartsNumber),
	    HLL_EXPORT(Parts_GetInputState, PE_GetInputState),
	    HLL_TODO_EXPORT(Parts_GetOnCursorShowLinkPartsNumber, PartsEngine_Parts_GetOnCursorShowLinkPartsNumber),
	    HLL_TODO_EXPORT(Parts_GetSoundNumber, PartsEngine_Parts_GetSoundNumber),
	    HLL_TODO_EXPORT(Parts_IsPartsPixelDecide, PartsEngine_Parts_IsPartsPixelDecide),
	    HLL_EXPORT(Parts_IsCursorIn, PE_IsCursorIn),
	    HLL_TODO_EXPORT(SaveParts, PartsEngine_SaveParts),
	    HLL_TODO_EXPORT(LoadParts, PartsEngine_LoadParts)
	    );

static struct ain_hll_function *get_fun(int libno, const char *name)
{
	int fno = ain_get_library_function(ain, libno, name);
	return fno >= 0 ? &ain->libraries[libno].functions[fno] : NULL;
}

static void PartsEngine_PreLink(void)
{
	struct ain_hll_function *fun;
	int libno = ain_get_library(ain, "PartsEngine");
	assert(libno >= 0);

	fun = get_fun(libno, "AddDrawCutCGToPartsConstructionProcess");
	if (fun && fun->nr_arguments == 12) {
		static_library_replace(&lib_PartsEngine, "AddDrawCutCGToPartsConstructionProcess",
				PE_AddDrawCutCGToPartsConstructionProcess);
	}
	fun = get_fun(libno, "AddCopyCutCGToPartsConstructionProcess");
	if (fun && fun->nr_arguments == 12) {
		static_library_replace(&lib_PartsEngine, "AddCopyCutCGToPartsConstructionProcess",
				PE_AddCopyCutCGToPartsConstructionProcess);
	}
	// Ixseal passes the part number, state and a fourth (point list) array
	// explicitly: AddPartsConstructionProcess(int, ArrayInt, ArrayFloat,
	// ArrayString, ArrayPos, int) instead of the classic three arrays.
	fun = get_fun(libno, "AddPartsConstructionProcess");
	if (fun && fun->nr_arguments == 6) {
		static_library_replace(&lib_PartsEngine, "AddPartsConstructionProcess",
				PE_AddPartsConstructionProcess_ix);
	}
	fun = get_fun(libno, "Update");
	if (fun && fun->nr_arguments == 5) {
		static_library_replace(&lib_PartsEngine, "Update",
				PartsEngine_Update_Pascha3PC);
	}
	if (get_fun(libno, "AddController")) {
		PE_enable_multi_controller();
	}
}
