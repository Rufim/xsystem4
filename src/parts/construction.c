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
#include "system4.h"
#include "system4/cg.h"
#include "system4/string.h"

#include "xsystem4.h"
#include "asset_manager.h"
#include "parts.h"
#include "parts_internal.h"

static struct parts_construction_process *get_cproc(int parts_no, int state)
{
	if (getenv("XSYS4_CP_TRACE") && parts_no >= 90000000)
		NOTICE("CP add-op on part=%d state=%d", parts_no, state);
	return parts_get_construction_process(parts_get(parts_no), state);
}

void parts_cp_op_free(struct parts_cp_op *op)
{
	switch (op->type) {
	case PARTS_CP_CREATE:
	case PARTS_CP_CREATE_PIXEL_ONLY:
	case PARTS_CP_CG:
	case PARTS_CP_FILL:
	case PARTS_CP_FILL_ALPHA_COLOR:
	case PARTS_CP_FILL_PIE_AMAP:
	case PARTS_CP_FILL_AMAP:
	case PARTS_CP_FILL_WITH_ALPHA:
	case PARTS_CP_DRAW_RECT:
	case PARTS_CP_DRAW_CUT_CG:
	case PARTS_CP_COPY_CUT_CG:
	case PARTS_CP_GRAY_FILTER:
	case PARTS_CP_FILL_GRADATION_HORIZON:
	case PARTS_CP_MUL_FILTER:
		break;
	case PARTS_CP_DRAW_TEXT:
	case PARTS_CP_COPY_TEXT:
		free_string(op->text.text);
		break;
	}
	free(op);
}

void parts_add_cp_op(struct parts_construction_process *cproc, struct parts_cp_op *op)
{
	TAILQ_INSERT_TAIL(&cproc->ops, op, entry);
}

bool PE_ClearPartsConstructionProcess(int parts_no, int state);

bool PE_AddCreateToPartsConstructionProcess(int parts_no, int w, int h, int state)
{
	if (getenv("XSYS4_CP_TRACE") && parts_no >= 90000000)
		NOTICE("CP AddCreate part=%d w=%d h=%d state=%d valid=%d",
		       parts_no, w, h, state, parts_state_valid(state-1));
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_CREATE;
	op->create.w = w;
	op->create.h = h;
	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddCreatePixelOnlyToPartsConstructionProcess(int parts_no, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_CREATE_PIXEL_ONLY;
	op->create.w = w;
	op->create.h = h;
	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddCreateCGToProcess(int parts_no, struct string *cg_name, int state)
{
	if (!parts_state_valid(--state))
		return false;

	int no;
	if (!asset_exists_by_name(ASSET_CG, cg_name->text, &no)) {
		WARNING("Invalid CG name: %s", display_sjis0(cg_name->text));
		return false;
	}

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_CG;
	op->cg.no = no;
	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddFillToPartsConstructionProcess(int parts_no, int x, int y, int w, int h, int r, int g, int b, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_FILL;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.r = r, .g = g, .b = b, .a = 255
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddFillAlphaColorToPartsConstructionProcess(int parts_no, int x, int y, int w, int h, int r, int g, int b, int a, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_FILL_ALPHA_COLOR;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.r = r, .g = g, .b = b, .a = a
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddFillAMapToPartsConstructionProcess(int parts_no, int x, int y, int w, int h, int a, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_FILL_AMAP;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.r = 0, .g = 0, .b = 0, .a = a
	};

	parts_add_cp_op(cproc, op);
	return true;
}

/*
 * `SetFillPieAMap` (команда 122) — сектор эллипса в АЛЬФА-КАРТУ. Скруглённые
 * подложки интерфейса Dohna строятся именно так: два прямоугольника крестом плюс
 * четыре четверти круга по углам (см. «Round128x40» в PlayerShopView.pactex).
 */
bool PE_AddFillPieAMapToPartsConstructionProcess(int parts_no, int x, int y, int rx, int ry,
		int start_angle, int sweep_angle, int a, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_FILL_PIE_AMAP;
	op->pie = (struct parts_cp_pie) {
		.x = x, .y = y, .rx = rx, .ry = ry,
		.start_angle = start_angle, .sweep_angle = sweep_angle, .a = a
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddFillWithAlphaToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, int a, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_FILL_WITH_ALPHA;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.r = r, .g = g, .b = b, .a = a
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddFillGradationHorizonToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int top_r, int top_g, int top_b, int bot_r, int bot_g, int bot_b, int state);

bool PE_AddDrawRectToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_DRAW_RECT;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.r = r, .g = g, .b = b, .a = 255
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddDrawCutCGToPartsConstructionProcess(int parts_no, struct string *cg_name,
		int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, int interp_type, int state)
{
	if (!parts_state_valid(--state))
		return false;

	int cg_no;
	if (!asset_exists_by_name(ASSET_CG, cg_name->text, &cg_no)) {
		WARNING("Invalid CG name: %s", display_sjis0(cg_name->text));
		return false;
	}

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_DRAW_CUT_CG;
	op->cut_cg = (struct parts_cp_cut_cg) {
		.cg_no = cg_no,
		.dx = dx, .dy = dy, .dw = dw, .dh = dh,
		.sx = sx, .sy = sy, .sw = sw, .sh = sh,
		.interp_type = interp_type
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddCopyCutCGToPartsConstructionProcess(int parts_no, struct string *cg_name,
		int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, int interp_type, int state)
{
	if (!parts_state_valid(--state))
		return false;

	int cg_no;
	if (!asset_exists_by_name(ASSET_CG, cg_name->text, &cg_no)) {
		WARNING("Invalid CG name: %s", display_sjis0(cg_name->text));
		return false;
	}

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_COPY_CUT_CG;
	op->cut_cg = (struct parts_cp_cut_cg) {
		.cg_no = cg_no,
		.dx = dx, .dy = dy, .dw = dw, .dh = dh,
		.sx = sx, .sy = sy, .sw = sw, .sh = sh,
		.interp_type = interp_type
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddGrayFilterToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		bool full_size, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_GRAY_FILTER;
	op->filter = (struct parts_cp_filter) {
		.x = x, .y = y, .w = w, .h = h,
		.full_size = full_size
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddAddFilterToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, bool full_size, int state);
bool PE_AddDrawLineToPartsConstructionProcess(int parts_no, int x1, int y1, int x2, int y2,
		int r, int g, int b, int a, int state);

bool PE_AddFillGradationHorizonToPartsConstructionProcess(int parts_no, int x, int y,
		int w, int h, int top_r, int top_g, int top_b, int bot_r, int bot_g, int bot_b,
		int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_FILL_GRADATION_HORIZON;
	op->gradation = (struct parts_cp_fill_gradation) {
		.x = x, .y = y, .w = w, .h = h,
		.top_r = top_r, .top_g = top_g, .top_b = top_b,
		.bot_r = bot_r, .bot_g = bot_g, .bot_b = bot_b
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddMulFilterToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, bool full_size, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_MUL_FILTER;
	op->color_filter = (struct parts_cp_color_filter) {
		.x = x, .y = y, .w = w, .h = h,
		.r = r, .g = g, .b = b,
		.full_size = full_size
	};

	parts_add_cp_op(cproc, op);
	return true;
}

static bool add_text_to_cproc(int parts_no, int x, int y, struct string *text,
		int type, int size, int r, int g, int b, float bold_weight,
		int edge_r, int edge_g, int edge_b, float edge_weight,
		int char_space, int line_space, int state, enum parts_cp_op_type op_type)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = op_type;
	op->text = (struct parts_cp_text) {
		.text = string_dup(text),
		.x = x,
		.y = y,
		.line_space = line_space,
		.style = {
			.face = type,
			.size = size,
			.bold_width = bold_weight,
			.weight = 0,
			.edge_left = edge_weight,
			.edge_up = edge_weight,
			.edge_right = edge_weight,
			.edge_down = edge_weight,
			.color = { r, g, b, 255 },
			.edge_color = { edge_r, edge_g, edge_b, 255 },
			.scale_x = 1.0f,
			.space_scale_x = 1.0f,
			.font_spacing = char_space
		}
	};

	parts_add_cp_op(get_cproc(parts_no, state), op);
	return true;

}


bool PE_AddDrawTextToPartsConstructionProcess(int parts_no, int x, int y, struct string *text,
		int type, int size, int r, int g, int b, float bold_weight,
		int edge_r, int edge_g, int edge_b, float edge_weight,
		int char_space, int line_space, int state)
{
	return add_text_to_cproc(parts_no, x, y, text, type, size, r, g, b, bold_weight,
			edge_r, edge_g, edge_b, edge_weight, char_space, line_space, state,
			PARTS_CP_DRAW_TEXT);
}

bool PE_AddCopyTextToPartsConstructionProcess(int parts_no, int x, int y, struct string *text,
		int type, int size, int r, int g, int b, float bold_weight,
		int edge_r, int edge_g, int edge_b, float edge_weight,
		int char_space, int line_space, int state)
{
	return add_text_to_cproc(parts_no, x, y, text, type, size, r, g, b, bold_weight,
			edge_r, edge_g, edge_b, edge_weight, char_space, line_space, state,
			PARTS_CP_COPY_TEXT);
}

static bool build_create(struct parts *parts, struct parts_construction_process *cproc,
		struct parts_cp_create *op)
{
	// ВЫРОДИВШАЯСЯ поверхность (нулевая ширина или высота) — единственный случай, когда
	// инициализация должна быть ПРОЗРАЧНОЙ. Бэклог просит разделитель как Width×0, у
	// оригинала он поэтому не виден вовсе; мы обязаны кламповать до 1×1 (текстура с
	// нулевым измерением имеет handle == 0 и валит `gfx_set_framebuffer` фаталом
	// «Incomplete framebuffer» при композиции), и вот этот 1 px и рисовался сплошной
	// чёрной линией.
	bool degenerate = op->w <= 0 || op->h <= 0;
	int w = op->w > 0 ? op->w : 1;
	int h = op->h > 0 ? op->h : 1;
	gfx_delete_texture(&cproc->common.texture);
	// ★Обычная поверхность создаётся НЕПРОЗРАЧНОЙ (как upstream). Прежняя правка гасила
	// альфу у ВСЕХ созданных поверхностей — так лечили те самые чёрные полосы бэклога, —
	// и заодно убила белую заливку под логотипом игры: команды заливки идут парой
	// «SetCreate(1024×768) → SetFill(255,255,255)», а `SetFill` по семантике красит только
	// RGB и альфу не трогает (`gfx_fill`), поэтому белое оставалось с alpha = 0 и экран
	// логотипа выходил чёрным вместо белого. Ручка XSYS4_TRANSPARENT_CREATE возвращает
	// прежнее поведение целиком.
	SDL_Color init = (degenerate || getenv("XSYS4_TRANSPARENT_CREATE"))
		? (SDL_Color){0,0,0,0} : (SDL_Color){0,0,0,255};
	gfx_init_texture_rgba(&cproc->common.texture, w, h, init);
	if (!cproc->common.texture.handle)
		return false;
	parts_set_dims(parts, &cproc->common, w, h);
	return true;
}

static bool build_create_pixel_only(struct parts *parts, struct parts_construction_process *cproc,
		struct parts_cp_create *op)
{
	int w = op->w > 0 ? op->w : 1;
	int h = op->h > 0 ? op->h : 1;
	gfx_delete_texture(&cproc->common.texture);
	gfx_init_texture_rgb(&cproc->common.texture, w, h, (SDL_Color){0,0,0,255});
	if (!cproc->common.texture.handle)
		return false;
	parts_set_dims(parts, &cproc->common, w, h);
	return true;
}

static bool build_cg(struct parts *parts, struct parts_construction_process *cproc, struct parts_cp_cg *op)
{
	struct cg *cg = asset_cg_load(op->no);
	if (!cg)
		return false;
	gfx_delete_texture(&cproc->common.texture);
	gfx_init_texture_with_cg(&cproc->common.texture, cg);
	parts_set_dims(parts, &cproc->common, cg->metrics.w, cg->metrics.h);
	cg_free(cg);
	return true;
}

static bool build_fill(struct parts_construction_process *cproc, struct parts_cp_fill *op)
{
	if (!cproc->common.texture.handle)
		return false;
	gfx_fill(&cproc->common.texture, op->x, op->y, op->w, op->h, op->r, op->g, op->b);
	return true;
}

static bool build_fill_alpha_color(struct parts_construction_process *cproc, struct parts_cp_fill *op)
{
	if (!cproc->common.texture.handle)
		return false;
	gfx_fill_alpha_color(&cproc->common.texture, op->x, op->y, op->w, op->h, op->r, op->g, op->b, op->a);
	return true;
}

/*
 * `全体 = 1` («на весь размер»): раскладка при этом флаге кладёт в `先矩形` нули,
 * а в рантайме флаг приходит отдельным полем FullSize — но размеры там точно так
 * же нулевые. Заливка нулевого прямоугольника не значит ничего (no-op), поэтому
 * нули трактуем как «вся поверхность»: без этого белая подложка «Round128x40»
 * оставалась чёрной — шаг `FillWithAlpha(255,255,255, a=0, 全体=1)` не срабатывал,
 * и RGB под альфа-маской оставался нулевым.
 */
static void cp_fill_rect(struct parts_construction_process *cproc, int *x, int *y, int *w, int *h)
{
	if (*w > 0 && *h > 0)
		return;
	if (getenv("XSYS4_CP_NO_FULLFILL"))  // A/B-ручка
		return;
	*x = 0;
	*y = 0;
	*w = cproc->common.texture.w;
	*h = cproc->common.texture.h;
}

static bool build_fill_amap(struct parts_construction_process *cproc, struct parts_cp_fill *op)
{
	if (!cproc->common.texture.handle)
		return false;
	int x = op->x, y = op->y, w = op->w, h = op->h;
	cp_fill_rect(cproc, &x, &y, &w, &h);
	gfx_fill_amap(&cproc->common.texture, x, y, w, h, op->a);
	return true;
}

/*
 * Растеризация сектора в альфа-карту. Пишем МАКСИМУМОМ (`gfx_copy_amap_max`):
 * фигура кладётся поверх уже собранной маски и не должна затирать соседние
 * куски прозрачностью за пределами дуги — у «Round128x40» углы дорисовываются
 * к уже залитому кресту.
 *
 * Края сглаживаем суперсэмплингом 3×3: без него скруглённые подложки заметно
 * «лесенкой» отличаются от оригинала.
 */
static bool build_fill_pie_amap(struct parts_construction_process *cproc, struct parts_cp_pie *op)
{
	if (!cproc->common.texture.handle)
		return false;
	int rx = abs(op->rx), ry = abs(op->ry);
	if (rx <= 0 || ry <= 0)
		return false;
	int w = rx * 2, h = ry * 2;
	uint8_t *amap = xcalloc(1, (size_t)w * h);
	float a0 = op->start_angle * (float)M_PI / 180.0f;
	float a1 = a0 + op->sweep_angle * (float)M_PI / 180.0f;
	if (a1 < a0) { float t = a0; a0 = a1; a1 = t; }
	float sweep = a1 - a0;
	const int SS = 3;
	for (int py = 0; py < h; py++) {
		for (int px = 0; px < w; px++) {
			int hits = 0;
			for (int sy = 0; sy < SS; sy++) {
				for (int sx = 0; sx < SS; sx++) {
					float fx = px + (sx + 0.5f) / SS - rx;
					float fy = py + (sy + 0.5f) / SS - ry;
					float nx = fx / rx, ny = fy / ry;
					if (nx * nx + ny * ny > 1.0f)
						continue;
					if (sweep < 2.0f * (float)M_PI - 0.001f) {
						// atan2(y, x) при экранном Y (вниз) уже даёт угол по
						// часовой стрелке — ровно соглашение раскладки.
						float ang = atan2f(fy, fx);
						while (ang < a0)
							ang += 2.0f * (float)M_PI;
						if (ang > a1)
							continue;
					}
					hits++;
				}
			}
			if (hits)
				amap[py * w + px] = op->a * hits / (SS * SS);
		}
	}
	Texture tmp;
	gfx_init_texture_amap(&tmp, w, h, amap, (SDL_Color){0, 0, 0, 0});
	gfx_copy_amap_max(&cproc->common.texture, op->x - rx, op->y - ry, &tmp, 0, 0, w, h);
	gfx_delete_texture(&tmp);
	free(amap);
	return true;
}

static bool build_fill_with_alpha(struct parts_construction_process *cproc, struct parts_cp_fill *op)
{
	if (!cproc->common.texture.handle)
		return false;
	int x = op->x, y = op->y, w = op->w, h = op->h;
	cp_fill_rect(cproc, &x, &y, &w, &h);
	gfx_fill_with_alpha(&cproc->common.texture, x, y, w, h, op->r, op->g, op->b, op->a);
	return true;
}

static bool build_draw_rect(struct parts_construction_process *cproc, struct parts_cp_fill *op)
{
	if (!cproc->common.texture.handle)
		return false;
	int x2 = op->x + op->w - 1;
	int y2 = op->y + op->h - 1;
	gfx_draw_line(&cproc->common.texture, op->x, op->y, x2, op->y, op->r, op->g, op->b);
	gfx_draw_line(&cproc->common.texture, op->x, op->y, op->x, y2, op->r, op->g, op->b);
	gfx_draw_line(&cproc->common.texture, x2, op->y, x2, y2, op->r, op->g, op->b);
	gfx_draw_line(&cproc->common.texture, op->x, y2, x2, y2, op->r, op->g, op->b);
	return true;
}

static bool build_draw_cut_cg(struct parts_construction_process *cproc, struct parts_cp_cut_cg *op)
{
	if (!cproc->common.texture.handle)
		return false;

	struct cg *cg = asset_cg_load(op->cg_no);
	assert(cg);

	Texture src;
	gfx_init_texture_with_cg(&src, cg);
	cg_free(cg);

	gfx_copy_stretch_blend_amap(&cproc->common.texture, op->dx, op->dy, op->dw, op->dh,
			&src, op->sx, op->sy, op->sw, op->sh);
	gfx_delete_texture(&src);
	return true;
}

static bool build_copy_cut_cg(struct parts_construction_process *cproc, struct parts_cp_cut_cg *op)
{
	if (!cproc->common.texture.handle)
		return false;

	struct cg *cg = asset_cg_load(op->cg_no);
	assert(cg);

	Texture src;
	gfx_init_texture_with_cg(&src, cg);
	cg_free(cg);

	gfx_copy_stretch_with_alpha_map(&cproc->common.texture, op->dx, op->dy, op->dw, op->dh,
			&src, op->sx, op->sy, op->sw, op->sh);
	gfx_delete_texture(&src);
	return true;
}

static bool build_draw_text(struct parts_construction_process *cproc, struct parts_cp_text *op)
{
	if (!cproc->common.texture.handle)
		return false;
	gfx_render_text(&cproc->common.texture, op->x, op->y, op->text->text, &op->style, true);
	return true;
}

static bool build_copy_text(struct parts_construction_process *cproc, struct parts_cp_text *op)
{
	if (!cproc->common.texture.handle)
		return false;
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("build_copy_text tex=%dx%d x=%d y=%d size=%d color=%d,%d,%d face=%u text='%s'",
		       cproc->common.texture.w, cproc->common.texture.h, op->x, op->y,
		       (int)op->style.size, op->style.color.r, op->style.color.g, op->style.color.b,
		       op->style.face, op->text ? display_sjis0(op->text->text) : "(nil)");
	if (!op->text)
		return true;
	// Boldness comes from the game, which reads it back from our
	// GetPartsTextFontProperty (.ex says 太さ = 0 for the message text, and the log
	// uses the message font). The old 0.8 default was guessed against a reference
	// rendered with the wrong .fnl face; it also fed ceilf(bold_width) = 1px a side
	// into per-glyph advance once the outline advance was restored. Override for
	// A/B comparison with XSYS4_LOG_BOLD.
	{
		const char *e = getenv("XSYS4_LOG_BOLD");
		if (e)
			op->style.bold_width = strtof(e, NULL);
	}
	// Multi-line text: the game joins a log message's wrapped lines with '\n' and
	// sizes the texture for all of them (parts::detail::TextParts_CalcSize counts
	// '\n' × pixel-height). gfx_render_text draws a SINGLE line, so we split on '\n'
	// and advance Y per line. Without this the whole message rendered on one line
	// and ran off the right edge of the box (Tsumamigui 3 BACK LOG). '\n' (0x0A)
	// never appears as a Shift-JIS trailing byte, so a byte-wise split is safe.
	int h = ceilf(op->style.size + op->style.edge_up + op->style.edge_down);
	// Per-line pitch = one line's pixel height + line_space, which is exactly how the
	// game sizes the texture: parts::detail::TextParts_CalcSize returns
	// nlines*height + (nlines-1)*line_space. Confirmed by XSYS4_CP_TRACE at font 48 —
	// the game allocates 675x50 for a one-line unit and 675x91 for a two-line one,
	// i.e. 50 + (-9) + 50 with line_space = -9. Dividing the texture height by the
	// line count instead (91/2 = 45) drifted every line after the first and clipped
	// the last one at the texture border.
	int line_h = (int)ceilf(text_style_height(&op->style)) + op->line_space;
	// Reserve room for the outline ABOVE the first line: nothing in the text renderer
	// pads vertically, so drawing at op->y clipped the outline at the texture border.
	// Horizontally there is nothing to add: _gfx_render_text already advances by
	// text_style_advance_padding before every glyph (restored in §5f ФИКС 3), which is
	// exactly the room the left outline needs. Insetting by edge_left here too pushed
	// the log's ink 2 px right of the original (замер: x=97 против 95).
	int ex = (int)ceilf(op->style.edge_left);
	int ey = (int)ceilf(op->style.edge_up);
	char *buf = xstrdup(op->text->text);
	// Clear the text area ONCE, before drawing. A per-line clear is wrong whenever
	// line_h < h — which is the normal case here, since line_space is negative
	// (-9 in Tsumamigui 3's log): consecutive lines overlap by h - line_h, so the
	// next line's clear erased the bottom of the line above it. That chopped the
	// 【 】 brackets around the speaker name in half, since they span nearly the
	// whole em while latin/cyrillic does not.
	int nlines = 1, maxw = 0;
	for (char *line = buf, *p = buf; ; p++) {
		if (*p != '\n' && *p != '\0')
			continue;
		bool end = (*p == '\0');
		char saved = *p;
		*p = '\0';
		int lw = ceilf(gfx_size_text(&op->style, line));
		if (lw > maxw)
			maxw = lw;
		*p = saved;
		if (end)
			break;
		nlines++;
		line = p + 1;
	}
	gfx_fill_with_alpha(&cproc->common.texture, op->x, op->y, maxw + 2 * ex,
			(nlines - 1) * line_h + h,
			op->style.edge_color.r, op->style.edge_color.g, op->style.edge_color.b, 0);
	int y = op->y;
	for (char *line = buf, *p = buf; ; p++) {
		if (*p != '\n' && *p != '\0')
			continue;
		bool end = (*p == '\0');
		*p = '\0';
		gfx_render_text(&cproc->common.texture, op->x, y + ey, line, &op->style, false);
		if (end)
			break;
		y += line_h;
		line = p + 1;
	}
	free(buf);
	return true;
}

static bool build_gray_filter(struct parts_construction_process *cproc, struct parts_cp_filter *op)
{
	if (!cproc->common.texture.handle)
		return false;
	int x = op->x, y = op->y, w = op->w, h = op->h;
	if (op->full_size) {
		x = 0; y = 0;
		w = cproc->common.texture.w;
		h = cproc->common.texture.h;
	}

	gfx_copy_grayscale(&cproc->common.texture, x, y, &cproc->common.texture, x, y, w, h);
	return true;
}

// Цветового градиента в gfx нет (`gfx_fill_amap_gradation_ud` красит АЛЬФУ и требует
// своего шейдера), поэтому кладём построчно обычным `gfx_fill`. Это не горячий путь:
// construction-процесс строится ОДИН раз в текстуру, а не каждый кадр, поэтому h вызовов
// заливки — единовременная плата, а не нагрузка на отрисовку.
static bool build_fill_gradation_horizon(struct parts_construction_process *cproc,
		struct parts_cp_fill_gradation *op)
{
	if (!cproc->common.texture.handle)
		return false;
	if (op->w <= 0 || op->h <= 0)
		return true;

	for (int i = 0; i < op->h; i++) {
		// Делим на h-1, чтобы НИЖНЯЯ строка получила ровно нижний цвет
		// (при делении на h последняя строка не дотягивала бы до него).
		int d = op->h > 1 ? op->h - 1 : 1;
		int r = op->top_r + (op->bot_r - op->top_r) * i / d;
		int g = op->top_g + (op->bot_g - op->top_g) * i / d;
		int b = op->top_b + (op->bot_b - op->top_b) * i / d;
		gfx_fill(&cproc->common.texture, op->x, op->y + i, op->w, 1, r, g, b);
	}
	return true;
}

static bool build_mul_filter(struct parts_construction_process *cproc,
		struct parts_cp_color_filter *op)
{
	if (!cproc->common.texture.handle)
		return false;
	int x = op->x, y = op->y, w = op->w, h = op->h;
	if (op->full_size) {
		x = 0; y = 0;
		w = cproc->common.texture.w;
		h = cproc->common.texture.h;
	}

	gfx_fill_multiply(&cproc->common.texture, x, y, w, h, op->r, op->g, op->b);
	return true;
}

bool parts_build_construction_process(struct parts *parts,
		struct parts_construction_process *cproc)
{
	struct parts_cp_op *op;
	TAILQ_FOREACH(op, &cproc->ops, entry) {
		switch (op->type) {
		case PARTS_CP_CREATE:
			if (!build_create(parts, cproc, &op->create))
				return false;
			break;
		case PARTS_CP_CREATE_PIXEL_ONLY:
			if (!build_create_pixel_only(parts, cproc, &op->create))
				return false;
			break;
		case PARTS_CP_CG:
			if (!build_cg(parts, cproc, &op->cg))
				return false;
			break;
		case PARTS_CP_FILL:
			if (!build_fill(cproc, &op->fill))
				return false;
			break;
		case PARTS_CP_FILL_ALPHA_COLOR:
			if (!build_fill_alpha_color(cproc, &op->fill))
				return false;
			break;
		case PARTS_CP_FILL_PIE_AMAP:
			if (!build_fill_pie_amap(cproc, &op->pie))
				return false;
			break;
		case PARTS_CP_FILL_AMAP:
			if (!build_fill_amap(cproc, &op->fill))
				return false;
			break;
		case PARTS_CP_FILL_WITH_ALPHA:
			if (!build_fill_with_alpha(cproc, &op->fill))
				return false;
			break;
		case PARTS_CP_DRAW_RECT:
			if (!build_draw_rect(cproc, &op->fill))
				return false;
			break;
		case PARTS_CP_DRAW_CUT_CG:
			if (!build_draw_cut_cg(cproc, &op->cut_cg))
				return false;
			break;
		case PARTS_CP_COPY_CUT_CG:
			if (!build_copy_cut_cg(cproc, &op->cut_cg))
				return false;
			break;
		case PARTS_CP_DRAW_TEXT:
			if (!build_draw_text(cproc, &op->text))
				return false;
			break;
		case PARTS_CP_COPY_TEXT:
			if (!build_copy_text(cproc, &op->text))
				return false;
			break;
		case PARTS_CP_GRAY_FILTER:
			if (!build_gray_filter(cproc, &op->filter))
				return false;
			break;
		case PARTS_CP_FILL_GRADATION_HORIZON:
			if (!build_fill_gradation_horizon(cproc, &op->gradation))
				return false;
			break;
		case PARTS_CP_MUL_FILTER:
			if (!build_mul_filter(cproc, &op->color_filter))
				return false;
			break;
		}
	}
	parts_dirty(parts);
	return true;
}

bool PE_BuildPartsConstructionProcess(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_construction_process *cproc = parts_get_construction_process(parts, state);
	bool r = parts_build_construction_process(parts, cproc);
	if (getenv("XSYS4_CP_TRACE") && parts_no >= 90000000)
		NOTICE("CP BUILD part=%d state=%d -> %d tex=%u(%dx%d)", parts_no, state, r,
		       cproc->common.texture.handle, cproc->common.texture.w, cproc->common.texture.h);
	return r;
}

bool parts_clear_construction_process(struct parts_construction_process *cproc)
{
	while (!TAILQ_EMPTY(&cproc->ops)) {
		struct parts_cp_op *op = TAILQ_FIRST(&cproc->ops);
		TAILQ_REMOVE(&cproc->ops, op, entry);
		parts_cp_op_free(op);
	}
	return true;
}

bool PE_ClearPartsConstructionProcess(int parts_no, int state)
{
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("ClearPartsConstructionProcess part=%d state=%d", parts_no, state);
	if (!parts_state_valid(--state))
		return false;
	struct parts *parts = parts_get(parts_no);
	struct parts_construction_process *cproc = parts_get_construction_process(parts, state);
	return parts_clear_construction_process(cproc);
}

/*
 * Непрозрачная заливка ПРЯМО в состояние `構築パーツ`. Загрузчик раскладок кладёт
 * её вместо результата непроработанной «процедуры построения»: как картинка она
 * не годится (её скрывает флаг construction_mask), но как ПРЯМОУГОЛЬНАЯ МАСКА
 * альфа-клиппера — вполне. Раньше для этого звался PE_SetPartsColorFill, но он
 * делает CG-состояние, и часть переставала быть 構築パーツ для игры: ассерт
 * `StandView.jaf:52: (nonnull) m_act.GetConstruction("PlayerC")`.
 */
void PE_SetPartsConstructionFill(int parts_no, int w, int h, int state)
{
	if (w <= 0 || h <= 0 || !parts_state_valid(--state))
		return;
	struct parts *parts = parts_get(parts_no);
	struct parts_construction_process *cproc = parts_get_construction_process(parts, state);
	gfx_delete_texture(&cproc->common.texture);
	gfx_init_texture_rgba(&cproc->common.texture, w, h, (SDL_Color){255, 255, 255, 255});
	parts_set_dims(parts, &cproc->common, w, h);
	parts_dirty(parts);
}

bool PE_SetPartsConstructionSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_construction_process *cproc = parts_get_construction_process(parts, state);
	parts_set_surface_area(parts, &cproc->common, x, y, w, h);
	return true;
}
