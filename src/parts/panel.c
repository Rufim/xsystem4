/* Copyright (C) 2026 xsystem4 contributors
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

/*
 * Панель (`パネル`) — component type 14 в расширенном перечислении System 4 v14
 * (Ixseal). Порядок типов снят с `parts::detail::GetComponentTypeName`:
 * …10 メッセージウィンドウ, 11 スピンボックス, 12 縦スライダーバー,
 * 13 横スライダーバー, **14 パネル**, 15 フォーム, 16 フォームグループ,
 * 17 ユーザコンポーネント.
 *
 * Это сплошной цветной прямоугольник: API даёт ровно размер и RGBA
 * (`SetPanelSize`, `SetPanelColor` + геттеры `GetPanelR/G/B/A`), поэтому
 * рисуется он собственной одноцветной текстурой по общему CG-пути рендера.
 * Первым его создаёт `SceneContext@CreatePanel` (сцена логотипа).
 *
 * АЛЬФА-ГРАДИЕНТ по краям (`SetPanelAlphaGradation{Top,Bottom,Left,Right}`)
 * ХРАНИТСЯ, но НЕ рисуется: у пары есть геттеры, значит значение обязано
 * возвращаться как записано, — а вот что именно значит число (альфа на самом
 * краю? ширина полосы затухания в пикселях?), по байткоду не устанавливается:
 * все сайты — это property-обёртки `CPanelParts@Gradation*::set/get` и
 * AFL-функции `ＰＥパネル＿*グラデーション設定`, ни одна из которых значение не
 * интерпретирует. Поэтому при первом НЕнулевом градиенте печатается
 * одноразовый WARNING вместо тихого гадания.
 */

#include <stdbool.h>
#include <stdlib.h>

#include "gfx/gfx.h"
#include "parts.h"
#include "parts_internal.h"
#include "xsystem4.h"

struct parts_panel *parts_get_panel(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_PANEL) {
		parts_state_reset(&parts->states[state], PARTS_PANEL);
	}
	return &parts->states[state].panel;
}

static struct parts_panel *panel_try_get(int parts_no, int state)
{
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || !parts_state_valid(state) || parts->states[state].type != PARTS_PANEL)
		return NULL;
	return &parts->states[state].panel;
}

static void panel_rebuild(struct parts *parts, struct parts_panel *p)
{
	gfx_delete_texture(&p->common.texture);
	if (p->w <= 0 || p->h <= 0) {
		parts_set_dims(parts, &p->common, 0, 0);
		parts_dirty(parts);
		return;
	}
	gfx_init_texture_rgba(&p->common.texture, p->w, p->h, p->color);
	parts_set_dims(parts, &p->common, p->w, p->h);
	parts_dirty(parts);
}

// Состояние у панельных функций в аргументах не передаётся: `CPanelParts`
// работает с состоянием по умолчанию (как и layout box).
#define PANEL_STATE 0

void PE_SetPanelSize(int parts_no, int w, int h)
{
	struct parts *parts = parts_get(parts_no);
	struct parts_panel *p = parts_get_panel(parts, PANEL_STATE);
	p->w = w;
	p->h = h;
	panel_rebuild(parts, p);
}

void PE_SetPanelColor(int parts_no, int r, int g, int b, int a)
{
	struct parts *parts = parts_get(parts_no);
	struct parts_panel *p = parts_get_panel(parts, PANEL_STATE);
	p->color = (SDL_Color) { r, g, b, a };
	panel_rebuild(parts, p);
}

int PE_GetPanelR(int parts_no)
{
	struct parts_panel *p = panel_try_get(parts_no, PANEL_STATE);
	return p ? p->color.r : 0;
}

int PE_GetPanelG(int parts_no)
{
	struct parts_panel *p = panel_try_get(parts_no, PANEL_STATE);
	return p ? p->color.g : 0;
}

int PE_GetPanelB(int parts_no)
{
	struct parts_panel *p = panel_try_get(parts_no, PANEL_STATE);
	return p ? p->color.b : 0;
}

int PE_GetPanelA(int parts_no)
{
	struct parts_panel *p = panel_try_get(parts_no, PANEL_STATE);
	return p ? p->color.a : 0;
}

// Значение сохраняем как есть (геттеры обязаны вернуть записанное), а вот
// рисовать градиент не берёмся — см. шапку файла.
static void panel_note_gradation(int value)
{
	static bool logged = false;
	if (value != 0 && !logged) {
		logged = true;
		WARNING("Панель: альфа-градиент по краю (=%d) сохранён, но не рисуется "
			"— смысл значения по байткоду не установлен", value);
	}
}

#define PANEL_GRADATION_SETTER(name, field)					\
	void PE_SetPanelAlphaGradation##name(int parts_no, int value)		\
	{									\
		struct parts *parts = parts_get(parts_no);			\
		parts_get_panel(parts, PANEL_STATE)->field = value;		\
		panel_note_gradation(value);					\
	}

PANEL_GRADATION_SETTER(Top, grad_top)
PANEL_GRADATION_SETTER(Bottom, grad_bottom)
PANEL_GRADATION_SETTER(Left, grad_left)
PANEL_GRADATION_SETTER(Right, grad_right)

int PE_GetPanelAlphaGradationTop(int parts_no)
{
	struct parts_panel *p = panel_try_get(parts_no, PANEL_STATE);
	return p ? p->grad_top : 0;
}

int PE_GetPanelAlphaGradationBottom(int parts_no)
{
	struct parts_panel *p = panel_try_get(parts_no, PANEL_STATE);
	return p ? p->grad_bottom : 0;
}

int PE_GetPanelAlphaGradationLeft(int parts_no)
{
	struct parts_panel *p = panel_try_get(parts_no, PANEL_STATE);
	return p ? p->grad_left : 0;
}

int PE_GetPanelAlphaGradationRight(int parts_no)
{
	struct parts_panel *p = panel_try_get(parts_no, PANEL_STATE);
	return p ? p->grad_right : 0;
}
