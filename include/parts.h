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

#ifndef SYSTEM4_PARTS_H
#define SYSTEM4_PARTS_H

#include <stdbool.h>

struct page;
struct string;

// parts.c
void PE_enable_multi_controller(void);
void PE_set_message_empty_type_minus_one(void);
void PE_set_message_types_ixseal(void);
bool PE_Init(void);
void PE_Reset(void);
// Текстовые поля ввода (TextBox/MultiTextBox) живут в src/hll/PartsEngine.c,
// но фокус и клавиши им приносит общий ввод партов.
void PE_textbox_click(int parts_no);
void PE_textbox_key(int vk);
// Сообщение «ввод подтверждён» (тип 25) текстовому полю.
void PE_SendFixedEvent(int parts_no);
void PE_Update(int passed_time, bool message_window_show);
void PE_UpdateComponent(int passed_time);
void PE_UpdateParts(int passed_time, bool is_skip, bool message_window_show);
void PE_SetDelegateIndex(int parts_no, int delegate_index);
void PE_SetEventID(int parts_no, int delegate_index, int unique_id);
int PE_GetDelegateIndex(int parts_no);
// Слой части по номеру, или −1, если части уже нет.
int PE_parts_controller_no(int parts_no);
// Диагностика реестра активностей (для дампа частей по kill -USR1).
bool pe_parts_in_activity(int parts_no);
// Имя активности, за которой числится часть, или NULL. Строка — во внутреннем
// буфере вызова, действительна до следующего вызова.
const char *pe_parts_activity_name(int parts_no);
// Имя УЗЛА раскладки, которым часть заведена в активности (`Total`, `Income`…),
// или NULL. Строка — во внутреннем буфере вызова.
const char *pe_parts_node_name(int parts_no);
// Сводка живых активностей: имя, сколько частей числится и сколько из них живо.
void pe_dump_activities(void);
bool PE_SetPartsCG(int parts_no, struct string *cg_name, int sprite_deform, int state);
bool PE_SetPartsCG_by_index(int parts_no, int cg_no, int sprite_deform, int state);
bool PE_SetPartsCG_by_string_index(int parts_no, struct string *cg_no,
		int sprite_deform, int state);
void PE_GetPartsCGName(int parts_no, struct string **cg_name, int state);
int PE_GetPartsCGDeform(int parts_no, int state);
void PE_SetComponentReverseLR(int parts_no, bool reverse);
void PE_SetComponentReverseTB(int parts_no, bool reverse);
bool PE_GetComponentReverseLR(int parts_no);
bool PE_GetComponentReverseTB(int parts_no);
bool PE_SetPartsCGSurfaceArea(int parts_no, int x, int y, int w, int h, int state);
void PE_GetPartsCGSurfaceArea(int parts_no, int *x, int *y, int *w, int *h, int state);
int PE_GetPartsCGNumber(int parts_no, int state);
bool PE_SetLoopCG_by_index(int parts_no, int cg_no, int nr_frames, int frame_time, int state);
bool PE_SetLoopCG(int parts_no, struct string *cg_name, int start_no, int nr_frames,
		int frame_time, int state);
bool PE_SetLoopCGSurfaceArea(int parts_no, int x, int y, int w, int h, int state);
bool PE_SetHGaugeCG(int parts_no, struct string *cg_name, int state);
bool PE_SetHGaugeCG_by_index(int parts_no, int cg_no, int state);
bool PE_SetHGaugeRate(int parts_no, float numerator, float denominator, int state);
bool PE_SetHGaugeRate_int(int parts_no, int numerator, int denominator, int state);
bool PE_SetHGaugeSurfaceArea(int parts_no, int x, int y, int w, int h, int state);
float PE_GetHGaugeNumerator(int parts_no, int state);
float PE_GetHGaugeDenominator(int parts_no, int state);
float PE_GetVGaugeNumerator(int parts_no, int state);
float PE_GetVGaugeDenominator(int parts_no, int state);
bool PE_SetVGaugeCG(int parts_no, struct string *cg_name, int state);
bool PE_SetVGaugeCG_by_index(int parts_no, int cg_no, int state);
bool PE_SetVGaugeRate(int parts_no, float numerator, float denominator, int state);
bool PE_SetVGaugeRate_int(int parts_no, int numerator, int denominator, int state);
bool PE_SetVGaugeSurfaceArea(int parts_no, int x, int y, int w, int h, int state);
// `Reverse`: шкала заполняется от противоположного края (H — справа, V — сверху).
void PE_SetHGaugeReverse(int parts_no, bool enable, int state);
bool PE_IsHGaugeReverse(int parts_no, int state);
void PE_SetVGaugeReverse(int parts_no, bool enable, int state);
bool PE_IsVGaugeReverse(int parts_no, int state);
// Дробь заполнения, которой игра задала калибр (она же её и читает обратно).
float PE_GetHGaugeNumerator(int parts_no, int state);
float PE_GetHGaugeDenominator(int parts_no, int state);
float PE_GetVGaugeNumerator(int parts_no, int state);
float PE_GetVGaugeDenominator(int parts_no, int state);
bool PE_SetNumeralCG(int parts_no, struct string *cg_name, int state);
bool PE_SetNumeralCG_by_index(int parts_no, int cg_no, int state);
bool PE_SetNumeralLinkedCGNumberWidthWidthList_by_index(int parts_no, int cg_no,
		int w0, int w1, int w2, int w3, int w4, int w5, int w6, int w7, int w8,
		int w9, int w_minus, int w_comma, int state);
bool PE_SetNumeralLinkedCGNumberWidthWidthList(int parts_no, struct string *cg_name,
		int w0, int w1, int w2, int w3, int w4, int w5, int w6, int w7, int w8,
		int w9, int w_minus, int w_comma, int state);
bool PE_SetNumeralNumber(int parts_no, int n, int state);
int PE_GetNumeralNumber(int parts_no, int state);
bool PE_IsNumeralShowComma(int parts_no, int state);
int PE_GetNumeralSpace(int parts_no, int state);
int PE_GetNumeralLength(int parts_no, int state);
bool PE_SetNumeralShowComma(int parts_no, bool show_comma, int state);
bool PE_SetNumeralSpace(int parts_no, int space, int state);
void PE_SetNumeralFont(int parts_no, int type, int size, int r, int g, int b,
		float bold_weight, int edge_r, int edge_g, int edge_b,
		float edge_weight, int state);
void PE_SetNumeralShowType(int parts_no, int type, int state);
bool PE_SetNumeralLength(int parts_no, int length, int state);
bool PE_SetNumeralSurfaceArea(int parts_no, int x, int y, int w, int h, int state);
void PE_ReleaseParts(int parts_no);
void PE_ReleaseAllParts(void);
void PE_ReleaseAllPartsWithoutSystem(void);
void PE_ReleaseAllWithoutSystem(struct page **erase_number_list);
void parts_debug_dump(void);
// Разовый дамп ПО СИГНАЛУ: обработчик SIGUSR1 только поднимает флаг, а печать идёт
// из главного цикла (из обработчика сигнала звать это нельзя). Нужен, когда баг
// ловится руками на живом прогоне и включить XSYS4_DUMP_PARTS заранее было нечем.
void parts_request_debug_dump(void);
int PE_GetComponentAbsoluteMaxPosZ(int comp);
void PE_SetPos(int parts_no, int x, int y);
int PE_GetPartsX(int parts_no);
int PE_GetPartsY(int parts_no);
void PE_SetZ(int parts_no, int z);
int PE_GetPartsZ(int parts_no);
void PE_SetShow(int parts_no, bool show);
bool PE_GetPartsShow(int parts_no);
void PE_SetAlpha(int parts_no, int alpha);
int PE_GetPartsAlpha(int parts_no);
void PE_SetPartsDrawFilter(int parts_no, int draw_filter);
int PE_GetPartsDrawFilter(int parts_no);
void PE_SetAddColor(int parts_no, int r, int g, int b);
void PE_GetAddColor(int parts_no, int *r, int *g, int *b);
void PE_SetMultiplyColor(int parts_no, int r, int g, int b);
// `減算色モード`: добавочный цвет вычитается вместо прибавления.
void PE_SetSubColorMode(int parts_no, bool enable);
bool PE_IsSubColorMode(int parts_no);
void PE_GetMultiplyColor(int parts_no, int *r, int *g, int *b);
int PE_GetPartsWidth(int parts_no, int state);
int PE_GetPartsHeight(int parts_no, int state);
int PE_GetPartsUpperLeftPosX(int parts_no, int state);
int PE_GetPartsUpperLeftPosY(int parts_no, int state);
void PE_SetPartsOriginPosMode(int parts_no, int origin_pos_mode);
int PE_GetPartsOriginPosMode(int parts_no);
void PE_SetParentPartsNumber(int parts_no, int parent_parts_no);
int PE_GetParentPartsNumber(int parts_no);
int PE_NumofChild(int parts_no);
int PE_GetChild(int parts_no, int index);
int PE_GetChildIndex(int parts_no, int child_no);
void PE_ClearChild(int parts_no);
void PE_RemoveChild(int parts_no, int child_no);
bool PE_SetPartsGroupNumber(int parts_no, int group_no);
void PE_SetPartsMessageWindowShowLink(int parts_no, bool message_window_show_link);
bool PE_GetPartsMessageWindowShowLink(int parts_no);
void PE_SetSpeedupRateByMessageSkip(int parts_no, int rate);
void PE_SetPartsMagX(int parts_no, float scale_x);
float PE_GetPartsMagX(int parts_no);
void PE_SetPartsMagY(int parts_no, float scale_y);
float PE_GetPartsMagY(int parts_no);
void PE_SetPartsRotateX(int parts_no, float rot_x);
void PE_SetPartsRotateY(int parts_no, float rot_y);
void PE_SetPartsRotateZ(int parts_no, float rot_z);
float PE_GetPartsRotateZ(int parts_no);
float PE_GetPartsRotateX(int parts_no);
float PE_GetPartsRotateY(int parts_no);
int PE_GetPartsAlphaClipperPartsNumber(int parts_no);
void PE_SetPartsAlphaClipperPartsNumber(int parts_no, int alpha_clipper_parts_no);
void PE_SetPartsPixelDecide(int PartsNumber, bool pixel_decide);
bool PE_SetThumbnailReductionSize(int reduction_size);
bool PE_SetThumbnailMode(bool Mode);
void PE_SetComponentType(int parts_no, int type, int state);
int PE_GetComponentType(int parts_no, int state);
void PE_SetUserComponentName(int parts_no, struct string *name);
struct string *PE_GetUserComponentName(int parts_no);
void PE_SetUserComponentData(int parts_no, struct string *key, struct string *value);
struct string *PE_GetUserComponentData(int parts_no, struct string *key);
void PE_SetInputState(int parts_no, int state);
void PE_SetComponentEnableClipArea(int parts_no, bool enable);
bool PE_IsComponentEnableClipArea(int parts_no);
void PE_SetComponentClipArea(int parts_no, int x, int y, int w, int h);
int PE_GetComponentClipAreaPosX(int parts_no);
int PE_GetComponentClipAreaPosY(int parts_no);
int PE_GetComponentClipAreaPosWidth(int parts_no);
int PE_GetComponentClipAreaPosHeight(int parts_no);
int PE_GetInputState(int parts_no);
void PE_SetPartsConstructionMask(int parts_no);
void PE_EnsureParts(int parts_no);
bool PE_SetPartsRectangleDetectionSize(int parts_no, int w, int h, int state);
bool PE_SetPartsCGDetectionSize(int parts_no, struct string *cg_name, int state);
bool PE_Save(struct page **buffer);
bool PE_SaveWithoutHideParts(struct page **buffer);
bool PE_Load(struct page **buffer);
bool PE_SaveBackScene(struct page **buffer);
bool PE_LoadBackScene(struct page **buffer);
void PE_ClearBackScene(void);
int PE_AddController(int index);
void PE_RemoveController(struct page **erase_number_list, int index);
bool PE_CreateParts3DLayerPluginID(int parts_no, int state);
int PE_GetParts3DLayerPluginID(int parts_no, int state);
bool PE_ReleaseParts3DLayerPluginID(int parts_no, int state);
// GUIEngine
int PE_GetFreeNumber(void);
bool PE_IsExist(int parts_no);
// PartsFunc interface
void PE_set_active_controller(int controller_no);
int PE_get_active_controller(void);
void PE_SetVScrollbarMoveSizeByButton(int parts_no, int size);
int PE_GetVScrollbarMoveSizeByButton(int parts_no);
int PE_get_controller_length(void);
int PE_get_controller_id(int index);
// Снимок бэк-сцены («画面保管») живёт КОПИЯМИ партов в своём пространстве номеров: у AliceSoft
// это отдельная библиотека партов, сосуществующая с живой (отсюда всё семейство
// `*ForBackScene` — те сеттеры адресуют копию).
// ★ЛОВУШКА, СТОИВШАЯ ПРОГОНА: смещение НЕ МОЖЕТ быть 1e9 — System4 сам нумерует парты от
// 1 000 000 000 (у Tsumamigui 3 подложка окна сообщений = 1000000002, посимвольные парты
// текста = 1000001xxx). При offset 1e9 снос «пространства бэк-сцены» уносил живую подложку
// окна сообщений: после выхода из сцены текст висел без плашки. Отсюда 1.1e9 — выше всего
// живого диапазона и ниже INT_MAX даже для самого большого номера (1.1e9 + 1.0000013e9 =
// 2.1000013e9 < 2.147e9). Плюс снос идёт по ЯВНОМУ флагу `back_scene_copy`, а не по диапазону.
#define BACK_SCENE_PARTS_OFFSET 1100000000

// «Компонент» в HLL адресует и парт, и слой-контроллер; различение по диапазону ID.
bool parts_controller_is_layer(int id);
void parts_controller_set_show(int id, bool show);
// Восстановить стек слоёв после загрузки сейва (в сейве лежит только его глубина).
void parts_controller_stack_restore(int nr, int active);
int PE_get_controller_index(int id);
// XSYS4_PART_WATCH=<номер>[,…] — следить за конкретными партами.
bool parts_watched(int parts_no);
// Взять часть под наблюдение НА ХОДУ (XSYS4_WATCH_ACT — по имени активности).
void parts_watch_add(int parts_no);
int parts_cg_watch_part(void);
bool parts_controller_get_show(int id);
int PE_get_system_controller(void);
void PE_parts_set_want_save(int parts_no, bool want_save);
void PE_parts_set_want_save_back_scene(int parts_no, bool want);
bool PE_parts_get_want_save_back_scene(int parts_no);
bool PE_save_thumbnail(struct string *filename, int thumbnail_width);
bool PE_init_parts_movie(int parts_no, int width, int height, int bg_r, int bg_g, int bg_b, int state);
int PE_get_movie_sprite(int parts_no, int state);
float PE_parts_get_absolute_x(int parts_no);
float PE_parts_get_absolute_y(int parts_no);
int PE_parts_get_absolute_z(int parts_no);
// `オンカーソル表示連動`: часть видна, только пока курсор над частью `target_parts_no`
// (см. on_cursor_show_link в parts_internal.h). Ставит загрузчик раскладок.
void PE_set_on_cursor_show_link(int parts_no, int target_parts_no);
void PE_set_component_scroll_pos_link(int parts_no, int link_parts_no, bool vertical);
int PE_get_component_scroll_pos_link(int parts_no, bool vertical);
void PE_parts_set_lock_input_state(int parts_no, bool lock);

// construction.c
bool PE_AddCreateToPartsConstructionProcess(int parts_no, int w, int h, int state);
bool PE_AddCreatePixelOnlyToPartsConstructionProcess(int parts_no, int w, int h, int state);
bool PE_AddCreateCGToProcess(int parts_no, struct string *cg_name, int state);
bool PE_AddFillToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, int state);
bool PE_AddFillAlphaColorToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, int a, int state);
bool PE_AddFillAMapToPartsConstructionProcess(int parts_no, int x, int y, int w, int h, int a, int state);
bool PE_AddFillCircleBlendToPartsConstructionProcess(int parts_no, int x, int y,
		int rx, int ry, int r, int g, int b, int a, int state);
bool PE_AddFillPieAMapToPartsConstructionProcess(int parts_no, int x, int y, int rx, int ry,
		int start_angle, int sweep_angle, int a, int state);
bool PE_AddFillWithAlphaToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, int a, int state);
/*
 * Заливка ФИГУРЫ в одном из пяти режимов записи (команды 88…127, см. таблицу
 * номеров в docs/FINDINGS.md). `mode` — `enum parts_cp_shape_mode`; круг, эллипс
 * и сектор идут одним «pie»-путём (у круга rx == ry, у полного круга sweep = 360).
 */
enum parts_cp_shape_mode {
	PARTS_CP_SHAPE_COLOR,        // RGB подменяется, альфа приёмника не трогается
	PARTS_CP_SHAPE_WITH_ALPHA,   // RGB и альфа подменяются целиком
	PARTS_CP_SHAPE_AMAP,         // меняется только альфа
	PARTS_CP_SHAPE_COLOR_BLEND,  // цвет подмешивается по своей альфе, альфа приёмника та же
	PARTS_CP_SHAPE_ALPHA_BLEND,  // альфа поднимается по маске, затем ложится цвет
};
bool PE_AddFillPieModeToPartsConstructionProcess(int parts_no, int mode, int x, int y,
		int rx, int ry, int start_angle, int sweep_angle,
		int r, int g, int b, int a, int state);
bool PE_AddFillPolygonModeToPartsConstructionProcess(int parts_no, int mode, const int *pts,
		int nr_pts, int r, int g, int b, int a, int round_corner, int angle, int state);
bool PE_AddDrawRectToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, int state);
bool PE_AddDrawCutCGToPartsConstructionProcess(int parts_no, struct string *cg_name,
		int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, int interp_type, int state);
bool PE_AddCopyCutCGToPartsConstructionProcess(int parts_no, struct string *cg_name,
		int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, int interp_type, int state);
bool PE_AddGrayFilterToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		bool full_size, int state);
// Размытие по одной оси (команды 27/28 раскладки): `vertical` различает V- и H-вариант,
// `radius` — поле `ブラー`.
bool PE_AddBlurFilterToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		bool full_size, int radius, bool vertical, int state);
bool PE_AddCopyTextToPartsConstructionProcess(int parts_no, int x, int y, struct string *text,
		int type, int size, int r, int g, int b, float bold_weight,
		int edge_r, int edge_g, int edge_b, float edge_weight,
		int char_space, int line_space, int state);
bool PE_AddDrawTextToPartsConstructionProcess(int parts_no, int x, int y, struct string *text,
		int type, int size, int r, int g, int b, float bold_weight,
		int edge_r, int edge_g, int edge_b, float edge_weight,
		int char_space, int line_space, int state);
bool PE_BuildPartsConstructionProcess(int parts_no, int state);
bool PE_ClearPartsConstructionProcess(int parts_no, int state);
void PE_SetPartsConstructionFill(int parts_no, int w, int h, int state);
bool PE_SetPartsConstructionSurfaceArea(int parts_no, int x, int y, int w, int h, int state);

// input.c
void PE_UpdateInputState(int passed_time);
void PE_SetPassCursor(int parts_no, bool pass);
bool PE_GetPartsPassCursor(int parts_no);
void PE_SetClickable(int parts_no, bool clickable);
void PE_ClearClickableBan(int parts_no);
void PE_SetPartsIsButton(int parts_no, bool is_button);
void PE_SetPartsPixelHitTest(int parts_no, bool enable);
void PE_SetEnableInputProcess(int parts_no, bool enable);
bool PE_IsEnableInputProcess(int parts_no);
void PE_SetEnableInput(bool enable);
bool PE_IsEnableInput(void);
void PE_SetPartsWheelable(int parts_no, bool wheelable);
void PE_SetPartsHScrollbarScrollRate(int parts_no, float rate);
void PE_SetHSliderBarScrollRate(int parts_no, float rate);
int PE_GetPartsConstructionProcessCount(int parts_no, int state);
// Запомнить сырьё операции построения (см. struct parts_cp_raw в parts_internal.h):
// игра читает процедуру обратно через GetPartsConstructionProcess.
void PE_SaveConstructionRaw(int parts_no, int state, union vm_value *ints,
		int nr_ints, union vm_value *floats, int nr_floats,
		union vm_value *strings, int nr_strings, union vm_value *pos, int nr_pos);
// То же, но из обычных массивов — для процедур, собранных ЗАГРУЗЧИКОМ РАСКЛАДОК
// (там нет ни страниц VM, ни строк в куче).
void PE_SaveConstructionRawValues(int parts_no, int state, const int *ints, int nr_ints,
		const float *floats, int nr_floats, struct string **strings, int nr_strings);
void PE_GetPartsConstructionProcess(int parts_no, int index, struct page **a_int,
		struct page **a_float, struct page **a_string, struct page **a_pos, int state);
float PE_GetHSliderBarScrollRate(int parts_no);
float PE_GetPartsHScrollbarScrollRate(int parts_no);
void PE_InitPartsHScrollbar(int parts_no, int base_x, int base_y,
		int length, int width, int total, int view, float rate);
void PE_InitPartsVScrollbar(int parts_no, int base_x, int base_y, int length, int width,
		int up_size, int down_size, int total, int view, float rate);
void PE_SetPartsVScrollbarRate(int parts_no, float rate);
void PE_OnVScrollbarDragged(int parts_no, float rate);
void PE_SetPartsCheckBoxChecked(int parts_no, bool checked);
bool PE_GetPartsCheckBoxChecked(int parts_no);
void PE_InitPartsCheckBox(int parts_no, struct string *cg_base, bool checked);
void PE_SetPartsColorFill(int parts_no, int w, int h);
void PE_SetPartsCheckBoxColor(int parts_no, int r, int g, int b);
// Доступность чекбокса: выключенный не переключается кликом и рисуется `／無効`
// (System Menu в ADV гасит лишние пункты, когда ярлыков набрано четыре).
void PE_SetPartsCheckBoxEnable(int parts_no, bool enable);
bool PE_GetPartsCheckBoxEnable(int parts_no);
int PE_GetPartsCheckBoxR(int parts_no);
int PE_GetPartsCheckBoxG(int parts_no);
int PE_GetPartsCheckBoxB(int parts_no);
bool PE_GetPartsClickable(int parts_no);
void PE_SetDrag(int parts_no, bool enable);
void PE_SetSwipeType(int parts_no, int type);
int PE_GetSwipeType(int parts_no);
void PE_SetPartsGroupDecideOnCursor(int group_no, bool decide_on_cursor);
void PE_SetPartsGroupDecideClick(int group_no, bool decide_click);
void PE_SetOnCursorShowLinkPartsNumber(int parts_no, int link_parts_no);
int PE_GetOnCursorShowLinkPartsNumber(int parts_no);
bool PE_SetPartsOnCursorSoundNumber(int parts_no, int sound_no);
bool PE_SetPartsClickSoundNumber(int parts_no, int sound_no);
bool PE_SetClickMissSoundNumber(int sound_no);
bool PE_AddFillGradationHorizonToPartsConstructionProcess(int parts_no, int x, int y,
		int w, int h, int top_r, int top_g, int top_b, int bot_r, int bot_g, int bot_b,
		int state);
bool PE_AddMulFilterToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, bool full_size, int state);
bool PE_AddAddFilterToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, bool full_size, int state);
bool PE_AddFillGradationAMapToPartsConstructionProcess(int parts_no, int x, int y,
		int w, int h, int a1, int a2, bool vertical, int state);
bool PE_AddTileCGToPartsConstructionProcess(int parts_no, int x, int y,
		int w, int h, struct string *cg_name, bool copy, int state);
void PE_Parts_SetSoundNumber(int parts_no, int sound_no, int state);
int PE_Parts_GetSoundNumber(int parts_no, int state);
void PE_BeginInput(void);
void PE_EndInput(void);
int PE_GetClickPartsNumber(void);
int PE_GetActiveParts(void);
bool PE_IsCursorIn(int parts_no, int mouse_x, int mouse_y, int state);

// message.c
void PE_ReleaseMessage(void);
void PE_PopMessage(void);
int PE_GetMessageType(void);
int PE_GetMessagePartsNumber(void);
int PE_GetMessageDelegateIndex(void);
int PE_GetMessageUniqueID(void);
int PE_GetMessageVariableCount(void);
int PE_GetMessageVariableType(int index);
int PE_GetMessageVariableInt(int index);
float PE_GetMessageVariableFloat(int index);
bool PE_GetMessageVariableBool(int index);
void PE_GetMessageVariableString(int index, struct string **out);

// motion.c
void PE_AddMotionPos(int parts_no, int begin_x, int begin_y, int end_x, int end_y, int begin_t, int end_t);
void PE_AddMotionPos_curve(int parts_no, int begin_x, int begin_y, int end_x, int end_y,
		int begin_t, int end_t, struct string *curve_name);
void PE_AddMotionAlpha(int parts_no, int begin_a, int end_a, int begin_t, int end_t);
void PE_AddMotionAlpha_curve(int parts_no, int begin_a, int end_a, int begin_t, int end_t,
		struct string *curve_name);
void PE_AddMotionCG_by_index(int parts_no, int begin_cg_no, int nr_cg, int begin_t, int end_t);
void PE_AddMotionHGaugeRate(int parts_no, float begin_numerator, float begin_denominator,
			    float end_numerator, float end_denominator, int begin_t, int end_t);
void PE_AddMotionHGaugeRate_curve(int parts_no, float begin_numerator, float begin_denominator,
			    float end_numerator, float end_denominator, int begin_t, int end_t,
			    struct string *curve_name);
void PE_AddMotionVGaugeRate(int parts_no, float begin_numerator, float begin_denominator,
			    float end_numerator, float end_denominator, int begin_t, int end_t);
void PE_AddMotionVGaugeRate_curve(int parts_no, float begin_numerator, float begin_denominator,
			    float end_numerator, float end_denominator, int begin_t, int end_t,
			    struct string *curve_name);
void PE_AddMotionNumeralNumber(int parts_no, int begin_n, int end_n, int begin_t, int end_t);
void PE_AddMotionNumeralNumber_curve(int parts_no, int begin_n, int end_n, int begin_t,
		int end_t, struct string *curve_name);
void PE_AddMotionMagX(int parts_no, float begin, float end, int begin_t, int end_t);
void PE_AddMotionMagX_curve(int parts_no, float begin, float end, int begin_t, int end_t,
		struct string *curve_name);
void PE_AddMotionMagY(int parts_no, float begin, float end, int begin_t, int end_t);
void PE_AddMotionMagY_curve(int parts_no, float begin, float end, int begin_t, int end_t,
		struct string *curve_name);
void PE_AddMotionRotateX(int parts_no, float begin, float end, int begin_t, int end_t);
void PE_AddMotionRotateX_curve(int parts_no, float begin, float end, int begin_t, int end_t,
		struct string *curve_name);
void PE_AddMotionRotateY(int parts_no, float begin, float end, int begin_t, int end_t);
void PE_AddMotionRotateY_curve(int parts_no, float begin, float end, int begin_t, int end_t,
		struct string *curve_name);
void PE_AddMotionRotateZ(int parts_no, float begin, float end, int begin_t, int end_t);
void PE_AddMotionRotateZ_curve(int parts_no, float begin, float end, int begin_t, int end_t,
		struct string *curve_name);
void PE_AddMotionVibrationSize(int parts_no, int begin_w, int begin_h, int begin_t, int end_t);
void PE_AddMotionSound(int sound_no, int begin_t);
void PE_AddWholeMotionVibrationSize(int begin_w, int begin_h, int begin_t, int end_t);
void PE_BeginMotion(void);
void PE_EndMotion(void);
void PE_SetMotionTime(int t);
void PE_SeekEndMotion(void);
void PE_UpdateMotionTime(int time, bool skip);
bool PE_IsMotion(void);
int PE_GetMotionEndTime(void);
void PE_PauseMotion(bool pause);

// text.c
bool PE_SetText(int parts_no, struct string *text, int state);
// Сделать состояние текстовым, ничего в него не кладя (см. src/parts/text.c).
void PE_MakeTextState(int parts_no, int state);
// Пометить часть как поле ввода (см. src/parts/parts.c).
void parts_mark_textbox(int parts_no);
// Пометить часть как группу радиокнопок (см. src/parts/parts.c).
void parts_mark_radio_box(int parts_no);
// Пометить полосу как слайдер (v14 12/13, см. src/parts/parts.c).
void parts_mark_slider_bar(int parts_no, bool horizontal);
// Пометить часть как корень активности (см. src/parts/parts.c, §5dz).
void parts_mark_activity_root(int parts_no);
// Записать кнопку в состав группы (см. src/parts/parts.c).
void parts_radio_box_add_child(int box_no, int child_no);
// Номер группы, которой принадлежит кнопка, и её индекс в ней (-1 = вне группы).
int parts_radio_box_number(int parts_no, int *index_out);
bool PE_AddPartsText(int parts_no, struct string *text, int state);
void PE_SetTextEnableTag(int parts_no, bool enable, int state);
bool PE_IsTextEnableTag(int parts_no, int state);
struct string *PE_GetTextPartsText(int parts_no, int state);
bool PE_SetPartsTextSurfaceArea(int parts_no, int x, int y, int w, int h, int state);
bool PE_SetFont(int parts_no, int type, int size, int r, int g, int b, float bold_weight, int edge_r, int edge_g, int edge_b, float edge_weight, int state);
bool PE_SetPartsFontType(int parts_no, int type, int state);
bool PE_SetPartsFontSize(int parts_no, int size, int state);
bool PE_SetPartsFontColor(int parts_no, int r, int g, int b, int state);
bool PE_SetPartsFontBoldWeight(int parts_no, float bold_weight, int state);
bool PE_SetPartsFontEdgeColor(int parts_no, int r, int g, int b, int state);
bool PE_SetPartsFontEdgeWeight(int parts_no, float edge_weight, int state);
bool PE_SetTextCharSpace(int parts_no, int char_space, int state);
bool PE_SetTextLineSpace(int parts_no, int line_space, int state);
int PE_GetTextCharSpace(int parts_no, int state);
int PE_GetTextLineSpace(int parts_no, int state);
void PE_GetTextFontProps(int parts_no, int state, int *type, int *size,
		int *r, int *g, int *b, float *weight, float *edge_weight,
		int *edge_r, int *edge_g, int *edge_b);

// flash.c
bool PE_ExistsFlashFile(struct string *flash_filename);
bool PE_SetPartsFlash(int parts_no, struct string *flash_filename, int state);
bool PE_IsPartsFlashEnd(int parts_no, int state);
int PE_GetPartsFlashCurrentFrameNumber(int parts_no, int state);
bool PE_BackPartsFlashBeginFrame(int parts_no, int state);
bool PE_StepPartsFlashFinalFrame(int parts_no, int state);
bool PE_SetPartsFlashAndStop(int parts_no, struct string *flash_filename, int state);
bool PE_StopPartsFlash(int parts_no, int state);
bool PE_StartPartsFlash(int parts_no, int state);
bool PE_GoFramePartsFlash(int parts_no, int frame_no, int state);
int PE_GetPartsFlashEndFrame(int parts_no, int state);

// flat.c
bool PE_ExistsFlatFile(struct string *filename);
bool PE_SetPartsFlat(int parts_no, struct string *filename, int state);
bool PE_IsPartsFlatEnd(int parts_no, int state);
int PE_GetPartsFlatCurrentFrameNumber(int parts_no, int state);
bool PE_BackPartsFlatBeginFrame(int parts_no, int state);
bool PE_StepPartsFlatFinalFrame(int parts_no, int state);
bool PE_SetPartsFlatSurfaceArea(int parts_no, int x, int y, int w, int h, int state);
bool PE_SetPartsFlatAndStop(int parts_no, struct string *filename, int state);
bool PE_StopPartsFlat(int parts_no, int state);
bool PE_StartPartsFlat(int parts_no, int state);
bool PE_GoFramePartsFlat(int parts_no, int frame_no, int state);
int PE_GetPartsFlatEndFrame(int parts_no, int state);

// layoutbox.c
void PE_SetLayoutBoxLayoutType(int parts_no, int type);
int PE_GetLayoutBoxLayoutType(int parts_no);
void PE_SetLayoutBoxReturn(int parts_no, bool return_flag, int return_size);
bool PE_IsLayoutBoxReturn(int parts_no);
int PE_GetLayoutBoxReturnSize(int parts_no);
void PE_SetLayoutBoxAlign(int parts_no, int align);
// Панель (Ixseal, component type 14) — см. src/parts/panel.c.
void PE_SetPanelSize(int parts_no, int w, int h);
void PE_SetPanelColor(int parts_no, int r, int g, int b, int a);
int PE_GetPanelR(int parts_no);
int PE_GetPanelG(int parts_no);
int PE_GetPanelB(int parts_no);
int PE_GetPanelA(int parts_no);
void PE_SetPanelAlphaGradationTop(int parts_no, int value);
void PE_SetPanelAlphaGradationBottom(int parts_no, int value);
void PE_SetPanelAlphaGradationLeft(int parts_no, int value);
void PE_SetPanelAlphaGradationRight(int parts_no, int value);
int PE_GetPanelAlphaGradationTop(int parts_no);
int PE_GetPanelAlphaGradationBottom(int parts_no);
int PE_GetPanelAlphaGradationLeft(int parts_no);
int PE_GetPanelAlphaGradationRight(int parts_no);
int PE_GetLayoutBoxAlign(int parts_no);
void PE_SetComponentMargin(int parts_no, int top, int bottom, int left, int right);
int PE_GetComponentMarginTop(int parts_no);
int PE_GetComponentMarginBottom(int parts_no);
int PE_GetComponentMarginLeft(int parts_no);
int PE_GetComponentMarginRight(int parts_no);
void PE_set_layoutbox_padding(int parts_no, int top, int bottom, int left, int right);
int PE_get_layoutbox_padding_top(int parts_no);
int PE_get_layoutbox_padding_bottom(int parts_no);
int PE_get_layoutbox_padding_left(int parts_no);
int PE_get_layoutbox_padding_right(int parts_no);

// message_window.c — окно реплик ADV (`メッセージウィンドウ`, тип компонента v14 = 10).
void PE_CreateMessageWindow(int parts_no, int text_parts_no, int mark_parts_no);
// Номер текстовой части окна реплик или -1: стиль шрифта у нас живёт в текстовом
// состоянии, поэтому вызовы, адресующие ОКНО, обязаны переадресоваться на неё.
int PE_GetMessageWindowTextParts(int parts_no);
void PE_SetKeyWaitCGName(int parts_no, struct string *name, int start_no, int nr_cg, int time_per_cg);
void PE_GetKeyWaitCGName(int parts_no, struct string **name, int *start_no, int *nr_cg, int *time_per_cg);
void PE_SetKeyWaitFlatName(int parts_no, struct string *name);
struct string *PE_GetKeyWaitFlatName(int parts_no);
void PE_SetKeyWaitPos(int parts_no, int x, int y, int z);
int PE_GetKeyWaitPosX(int parts_no);
int PE_GetKeyWaitPosY(int parts_no);
int PE_GetKeyWaitPosZ(int parts_no);
void PE_SetKeyWaitShow(int parts_no, bool show);
bool PE_IsKeyWaitShow(int parts_no);
void PE_SetMessageWindowActive(int parts_no, bool active);
void PE_SetMessageWindowInactiveMultipleColor(int parts_no, int r, int g, int b);
int PE_GetMessageWindowInactiveMultipleColorR(int parts_no);
int PE_GetMessageWindowInactiveMultipleColorG(int parts_no);
int PE_GetMessageWindowInactiveMultipleColorB(int parts_no);
void PE_SetMessageWindowCGName(int parts_no, struct string *name);
struct string *PE_GetMessageWindowCGName(int parts_no);
void PE_SetMessageWindowFlatName(int parts_no, struct string *name);
struct string *PE_GetMessageWindowFlatName(int parts_no);
void PE_SetMessageWindowFlatShowWaitFrameNumber(int parts_no, int frame);
int PE_GetMessageWindowFlatShowWaitFrameNumber(int parts_no);
bool PE_IsOverMessageWindowFlatShowWaitFrame(int parts_no);
bool PE_BackMessageWindowFlatBeginFrame(int parts_no);
bool PE_StepMessageWindowFlatFinalFrame(int parts_no);
void PE_SetMessageWindowText(int parts_no, struct string *text, int msg_num,
                             struct string *func_name, int ver, int step);
struct string *PE_GetMessageWindowText(int parts_no);
void PE_FixMessageWindowText(int parts_no);
bool PE_IsFixedMessageWindowText(int parts_no);
void PE_SetMessageWindowTextArea(int parts_no, int x, int y, int w, int h);
void PE_GetMessageWindowTextArea(int parts_no, int *x, int *y, int *w, int *h);
void PE_SetMessageWindowTextOriginPosMode(int parts_no, int mode);
int PE_GetMessageWindowTextOriginPosMode(int parts_no);
void PE_SetMessageWindowTextFont(int parts_no, int type, int size, int r, int g, int b,
                                 float bold_weight, int edge_r, int edge_g, int edge_b,
                                 float edge_weight);
void PE_GetMessageWindowTextFont(int parts_no, int *type, int *size, int *r, int *g, int *b,
                                 float *bold_weight, int *edge_r, int *edge_g, int *edge_b,
                                 float *edge_weight);
void PE_SetMessageWindowTextSpeed(int parts_no, int speed);
int PE_GetMessageWindowTextSpeed(int parts_no);
void PE_SetMessageWindowTextSpace(int parts_no, int letter_space, int line_space);
void PE_GetMessageWindowTextSpace(int parts_no, int *letter_space, int *line_space);
void PE_SetMessageWindowRubyFont(int parts_no, int type, int size, int r, int g, int b,
                                 float bold_weight, int edge_r, int edge_g, int edge_b,
                                 float edge_weight);
void PE_GetMessageWindowRubyFont(int parts_no, int *type, int *size, int *r, int *g, int *b,
                                 float *bold_weight, int *edge_r, int *edge_g, int *edge_b,
                                 float *edge_weight);
void PE_SetMessageWindowRubyCharSpace(int parts_no, int space);
int PE_GetMessageWindowRubyCharSpace(int parts_no);
void PE_SetMessageWindowRubyLineSpace(int parts_no, int space);
int PE_GetMessageWindowRubyLineSpace(int parts_no);
void PE_SetEnableMessageWindowTextWrapping(int parts_no, bool enable);
bool PE_IsEnableMessageWindowTextWrapping(int parts_no);

#endif /* SYSTEM4_PARTS_H */
