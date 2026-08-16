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

#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>

#include <SDL.h>

#include "system4/ain.h"
#include "system4/file.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "cJSON.h"
#include "vm/page.h"
#include "iarray.h"
#include "input.h"
#include "gfx/gfx.h"
#include "mixer.h"
#include "savedata.h"
#include "vm.h"
#include "xsystem4.h"
#include "hll.h"
#include "system4/file.h"

static int SystemService_GetMixerName(int n, struct string **name)
{
	const char *r = mixer_get_name(n);
	if (!r)
		return 0;
	*name = make_string(r, strlen(r));
	return 1;
}

static bool SystemService_GetMixerDefaultVolume(int n, int *volume)
{
	// Граница — по фактическому числу микшеров (mixer_get_default_volume).
	// Прежде считалась по длине ini-списка, а без ключа `VolumeValancer` она
	// НОЛЬ, поэтому геттер проваливался всегда и игра видела громкость 0,
	// что она трактует как мьют (FINDINGS §5ag).
	return mixer_get_default_volume(n, volume);
}

static bool SystemService_SetMixerName(int n, struct string *name)
{
	return mixer_set_name(n, name->text);
}

static int SystemService_GetGameVersion(void)
{
	return ain->game_version;
}

/*
 * Тип платформы, на которой запущена игра. У AliceSoft этим гейтятся платформенные
 * различия (у DL-версий под Windows — обычный ПК-путь). Возвращаем 0: ровно это
 * значение подставлял `XSYS4_LENIENT_HLL`, и на нём Haha Ranman отыгрывает титул и
 * пролог; без функции же она валилась в debug-REPL на первом экране, то есть
 * запустить её можно было только костылём.
 */
static int SystemService_GetPlatformType(void)
{
	return 0;
}

/*
 * Хук закрытия приложения: у оригинала при HookCloseApp=true крестик окна не
 * закрывает игру сразу, а отдаёт событие игре (диалог «выйти?»). Наш движок
 * обрабатывает закрытие сам, поэтому честно храним только сам флаг — игра
 * ставит/снимает его в пост-загрузочном восстановлении (ロード後復帰処理),
 * и без функции загрузка сейва валилась фатальной Unimplemented.
 */
static bool hook_close_app = false;

static void SystemService_SetHookCloseApp(bool hook)
{
	hook_close_app = hook;
}

static bool SystemService_IsHookCloseApp(void)
{
	return hook_close_app;
}

/*
 * Анти-тамппер: `main` сверяет строку версии рантайма System4 по фиксированным
 * позициям — склеивает несколько `String.GetPart(v, index, len)` и сравнивает со
 * своей строковой константой, а рядом требует `ToInt(v[i]) == ToInt(v[j])`. Не
 * сошлось — `main` уходит в пустой цикл (чёрный экран).
 *
 * Строка у каждой сборки рантайма СВОЯ, поэтому один водяной знак на все игры
 * невозможен (у Dohna `[0:3]`=="sd4", у Healing Touch — "fea"). Ниже — снятые с
 * байткода наборы ограничений; нужный выбирается по наличию ожидаемой константы
 * в строковой таблице .ain, а сама строка синтезируется из ограничений.
 */
struct version_check {
	const char *expect;                    // константа, с которой сверяет main
	struct { int index, len; } frag[4];     // фрагменты в порядке конкатенации
	int eq_a, eq_b;                        // требование ToInt(v[a]) == ToInt(v[b])
};

static const struct version_check version_checks[] = {
	// Tsumamigui 3 (v7), main: [6:9] + [13:16] + [20:25], плюс [5] == [32].
	{ "]:77d66sadc", { {6,3}, {13,3}, {20,5} }, 5, 32 },
	// Dohna Dohna (v14), main @0x49a26c: [6:9] + [0:3] + [16:20]; @0x49a08a: [2] == [24].
	{ "6sdsd48f63", { {6,3}, {0,3}, {16,4} }, 2, 24 },
	// Healing Touch (v14, оба маршрута — проверки идентичны), main @0x49a7a0:
	// [5:8] + [13:16] + [0:3]; @0x49a5be: [16] == [3].
	{ "b544iofea", { {5,3}, {13,3}, {0,3} }, 16, 3 },
	// Haha Ranman (v14.0), main @0x4d3722: [5:7] + [20:22] + [1:4];
	// @0x4d3506: [6] == [17].
	{ "83h2mjr", { {5,2}, {20,2}, {1,3} }, 6, 17 },
};

static const char *game_version_text(void)
{
	static char buf[64];
	static bool built = false;
	if (built)
		return buf;
	built = true;
	memset(buf, '0', sizeof(buf));
	// Длина 33 — как у строки, на которой проверка Tsumamigui 3 уже проверена
	// работающей; самый большой нужный индекс — 32 (её же `[5]==[32]`).
	buf[33] = '\0';

	for (size_t v = 0; v < sizeof(version_checks) / sizeof(*version_checks); v++) {
		const struct version_check *vc = &version_checks[v];
		bool found = false;
		for (int i = 0; i < ain->nr_strings && !found; i++)
			found = !strcmp(ain->strings[i]->text, vc->expect);
		if (!found)
			continue;
		bool covered[sizeof(buf)];
		memset(covered, 0, sizeof(covered));
		const char *p = vc->expect;
		for (int f = 0; f < 4 && vc->frag[f].len; f++) {
			for (int k = 0; k < vc->frag[f].len; k++) {
				int at = vc->frag[f].index + k;
				if (at < 0 || at >= 33 || !*p)
					break;
				buf[at] = *p++;
				covered[at] = true;
			}
		}
		// `ToInt(v[a]) == ToInt(v[b])`: подгоняем ту позицию, которую фрагменты
		// не задали (если не задана ни одна, обе уже равны — там наполнитель).
		if (buf[vc->eq_a] != buf[vc->eq_b]) {
			if (!covered[vc->eq_b])
				buf[vc->eq_b] = buf[vc->eq_a];
			else if (!covered[vc->eq_a])
				buf[vc->eq_a] = buf[vc->eq_b];
			else
				WARNING("версия рантайма: ограничения [%d]==[%d] и фрагменты "
					"противоречивы", vc->eq_a, vc->eq_b);
		}
		return buf;
	}
	WARNING("анти-тамппер: набор ограничений для этой игры не известен — "
		"main может уйти в пустой цикл");
	return buf;
}

// v7-форма: out-параметр `ref string`, возвращает bool.
static bool SystemService_GetGameVersionByText(struct string **text)
{
	if (*text)
		free_string(*text);
	*text = cstr_to_string(game_version_text());
	return true;
}

// Ixseal-форма (v14): без аргументов, строка — это ВОЗВРАТ (`ret=12 ()`).
// Прежде она линковалась на v7-функцию: cif собирался по объявлению .ain, т.е.
// без аргументов и с указателем-возвратом, а C-функция читала `text` из
// неинициализированного регистра и отдавала `true` — движок оборачивал 1 как
// `struct string *` и падал в `sjis_count_char(0x9)` при первом же GetPart.
static struct string *SystemService_GetGameVersionByText_ix(void)
{
	return cstr_to_string(game_version_text());
}

static void SystemService_GetGameName(struct string **game_name)
{
	if (*game_name)
		free_string(*game_name);
	*game_name = cstr_to_string(config.game_name);
}

// Ixseal-формы: в v14 `GetGameName`/`GetGameFolderPath` объявлены `ret=12 ()`,
// т.е. строка — ВОЗВРАТ, а не out-параметр (у v6/v7 out-параметр). Линковка идёт
// по имени, cif строится по .ain, поэтому v7-функция получала мусорный
// указатель в первом регистре и ПИСАЛА по нему, а возвращала мусор: слот строки
// оказывался с `s = NULL`, и первый же `A_REF` падал в `string_ref(NULL)`
// (`ExtableFormatLoader@Load` @0x5807f6). Тот же класс, что у
// GetGameVersionByText (см. выше). Подмена — в _PreLink по nr_arguments == 0.
static struct string *SystemService_GetGameName_ix(void)
{
	return cstr_to_string(config.game_name);
}


/*
 * Хранилища именованных строковых переменных (Ixseal): `SystemVariable_*` и
 * `GameVariable_*` — два независимых словаря ключ→значение с одинаковым API
 * (IsExist/Set/Get/NumofKey/GetKey/Erase). У v6/v7 этих функций нет вообще
 * (проверено ainliball по трём .ain), так что ветка Ixseal-only.
 *
 * Зачем они игре, видно по единственному потребителю — `ResetLoadManager`:
 * перед перезапуском VM он кладёт `ResetLoadTargetIndex`/`ResetLoadType`
 * (`Run` @0x61c448), а после — читает и СТИРАЕТ их (`LoadObject` @0x61c516,
 * ключа нет → сразу `return 0`). То есть требуется пережить vm-reset, а не
 * перезапуск процесса; сохранение на диск ни одним сайтом не подтверждается,
 * поэтому словари живут в памяти. Порядок ключей — порядок вставки: его
 * задаёт пара NumofKey/GetKey, другого способа перечислить ключи нет.
 */
struct sv_entry {
	struct string *key;
	struct string *value;
};

struct sv_store {
	struct sv_entry *entries;
	int nr_entries;
	int cap;
};

static struct sv_store sv_system, sv_game;

static int sv_find(struct sv_store *st, struct string *key)
{
	for (int i = 0; i < st->nr_entries; i++) {
		if (!strcmp(st->entries[i].key->text, key->text))
			return i;
	}
	return -1;
}

static void sv_set(struct sv_store *st, struct string *key, struct string *value)
{
	int i = sv_find(st, key);
	if (i >= 0) {
		free_string(st->entries[i].value);
		st->entries[i].value = string_ref(value);
		return;
	}
	if (st->nr_entries == st->cap) {
		st->cap = st->cap ? st->cap * 2 : 8;
		st->entries = xrealloc(st->entries, st->cap * sizeof(struct sv_entry));
	}
	st->entries[st->nr_entries].key = string_ref(key);
	st->entries[st->nr_entries].value = string_ref(value);
	st->nr_entries++;
}

static struct string *sv_get(struct sv_store *st, struct string *key)
{
	int i = sv_find(st, key);
	return string_ref(i >= 0 ? st->entries[i].value : &EMPTY_STRING);
}

static struct string *sv_get_key(struct sv_store *st, int index)
{
	if (index < 0 || index >= st->nr_entries)
		return string_ref(&EMPTY_STRING);
	return string_ref(st->entries[index].key);
}

static void sv_erase(struct sv_store *st, struct string *key)
{
	int i = sv_find(st, key);
	if (i < 0)
		return;
	free_string(st->entries[i].key);
	free_string(st->entries[i].value);
	memmove(&st->entries[i], &st->entries[i + 1],
		(st->nr_entries - i - 1) * sizeof(struct sv_entry));
	st->nr_entries--;
}

static bool SystemService_SystemVariable_IsExist(struct string *key) { return sv_find(&sv_system, key) >= 0; }
static void SystemService_SystemVariable_Set(struct string *key, struct string *value) { sv_set(&sv_system, key, value); }
static struct string *SystemService_SystemVariable_Get(struct string *key) { return sv_get(&sv_system, key); }
static int SystemService_SystemVariable_NumofKey(void) { return sv_system.nr_entries; }
static struct string *SystemService_SystemVariable_GetKey(int index) { return sv_get_key(&sv_system, index); }
static void SystemService_SystemVariable_Erase(struct string *key) { sv_erase(&sv_system, key); }

static bool SystemService_GameVariable_IsExist(struct string *key) { return sv_find(&sv_game, key) >= 0; }
static void SystemService_GameVariable_Set(struct string *key, struct string *value) { sv_set(&sv_game, key, value); }
static struct string *SystemService_GameVariable_Get(struct string *key) { return sv_get(&sv_game, key); }
static int SystemService_GameVariable_NumofKey(void) { return sv_game.nr_entries; }
static struct string *SystemService_GameVariable_GetKey(int index) { return sv_get_key(&sv_game, index); }
static void SystemService_GameVariable_Erase(struct string *key) { sv_erase(&sv_game, key); }

HLL_WARN_UNIMPLEMENTED(false, bool, SystemService, AddURLMenu, struct string *title, struct string *url);

static bool SystemService_IsFullScreen(void)
{
	return gfx_is_fullscreen();
}

static bool SystemService_ChangeNormalScreen(void)
{
	return gfx_set_fullscreen(false);
}

static bool SystemService_ChangeFullScreen(void)
{
	return gfx_set_fullscreen(true);
}

HLL_WARN_UNIMPLEMENTED(false, bool, SystemService, InitMainWindowPosAndSize);

//static bool SystemService_UpdateView(void);
HLL_QUIET_UNIMPLEMENTED(false, bool, SystemService, UpdateView);

static int SystemService_GetViewWidth(void)
{
	return config.view_width;
}

static int SystemService_GetViewHeight(void)
{
	return config.view_height;
}

// Newer games query the "default" view size to lay out their UI. When left
// unimplemented these return 0, so the game computes bogus (off-screen) parts
// positions. Return the configured view size (same as GetViewWidth/Height).
static int SystemService_GetDefaultViewWidth(void)
{
	return config.view_width;
}

static int SystemService_GetDefaultViewHeight(void)
{
	return config.view_height;
}

static bool SystemService_MoveMouseCursorPosImmediately(int x, int y)
{
	mouse_set_pos(x, y);
	return true;
}

static bool SystemService_SetHideMouseCursorByGame(bool hide)
{
	return mouse_show_cursor(!hide);
}

//bool SystemService_GetHideMouseCursorByGame(void);
HLL_WARN_UNIMPLEMENTED(false, bool, SystemService, SetUsePower2Texture, bool use);
//bool SystemService_GetUsePower2Texture(void);
HLL_WARN_UNIMPLEMENTED(true, bool, SystemService, SetAntiAliasingMode, int mode);
//int SystemService_GetAntiAliasingMode(void);
// Escalayer Reboot anti-tamper gate: main() spins calling this until the ref
// string it fills is 14 chars long AND text[0]+text[2]+text[4]+text[9..11]
// spells "kkrsre" (verified by disassembling the spin loop at 0x786d4). The
// real function returns a game watermark; we return any 14-char string that
// satisfies the check so the loop exits into game_main. Called ~1M/frame, so
// it must be cheap — reuse a cached string.
static void SystemService_EscalayerReboot011116(struct string **s)
{
	if (*s)
		free_string(*s);
	//              0123456789012 3   -> [0]=k [2]=k [4]=r [9]=s [10]=r [11]=e, len 14
	*s = make_string("k0k0r0000sre00", 14);
}
// View/платформенные хинты новых игр — тихие no-op / разумные дефолты.
HLL_QUIET_UNIMPLEMENTED(, void, SystemService, SetAndroidViewKeepScreen, bool keep);
HLL_QUIET_UNIMPLEMENTED(, void, SystemService, SetAndroidViewOrientation, int orient);
HLL_QUIET_UNIMPLEMENTED(, void, SystemService, SetViewResizableMode, bool mode);
HLL_QUIET_UNIMPLEMENTED(false, bool, SystemService, IsViewResizableMode);
HLL_QUIET_UNIMPLEMENTED(true, bool, SystemService, SetWaitVSyncMode, int mode);
HLL_QUIET_UNIMPLEMENTED(0, int, SystemService, GetWaitVSyncMode);
HLL_QUIET_UNIMPLEMENTED(false, bool, SystemService, GetUsePower2Texture);
HLL_QUIET_UNIMPLEMENTED(0, int, SystemService, GetAntiAliasingMode);

enum window_settings_asect_ratio {
	ASPECT_RATIO_NORMAL,
	ASPECT_RATIO_FIXED
};

enum window_settings_scaling_type {
	SCALING_NORMAL,
	SCALING_BICUBIC,
};

struct window_settings {
	enum window_settings_asect_ratio aspect_ratio;
	enum window_settings_scaling_type scaling_type;
	bool wait_vsync;
	bool record_pos_size;
	bool minimize_by_full_screen_inactive;
	bool back_to_title_confirm;
	bool close_game_confirm;
};

static struct window_settings window_settings = {
	.aspect_ratio = ASPECT_RATIO_NORMAL,
	.scaling_type = SCALING_NORMAL,
	.wait_vsync = false,
	.record_pos_size = false,
	.minimize_by_full_screen_inactive = true,
	.back_to_title_confirm = true,
	.close_game_confirm = true,
};

enum window_settings_id {
	WINDOW_SETTINGS_ASPECT_RATIO = 0,
	WINDOW_SETTINGS_SCALING_TYPE = 1,
	WINDOW_SETTINGS_WAIT_VSYNC = 2,
	WINDOW_SETTINGS_RECORD_POS_SIZE = 3,
	WINDOW_SETTINGS_MINIMIZE_BY_FULL_SCREEN_INACTIVE = 4,
	WINDOW_SETTINGS_BACK_TO_TITLE_CONFIRM = 5,
	WINDOW_SETTINGS_CLOSE_GAME_CONFIRM = 6,
};

/*
 * У новых релизов (ain 14, Ixseal) таблица настроек окна КОРОЧЕ: из неё выпали
 * «ожидать vsync» и «запоминать позицию/размер», и номера сдвинулись —
 * 2 = минимизировать в фоне, 3 = спрашивать при возврате на титул,
 * 4 = спрашивать при выходе. Замер по всем обращениям Dohna к
 * Get/SetWindowSetting (пары `config::detail::Get*`/`Set*` сходятся):
 *
 *   0 AspectRatio · 1 ScalingType · 2 MinimizeByFullScreenInactive
 *   3 BackToTitleConfirm · 4 CloseGameConfirm
 *
 * Из-за расхождения `config::detail::GetCloseGameConfirm` читал у нас
 * «минимизировать в фоне», а `GetBackToTitleConfirm` — «запоминать позицию»:
 * подтверждение оказывалось выключенным, и CONFIG → «Return to Title» уходил на
 * титул БЕЗ системного окна `タイトル画面に戻りますか？`, которое показывает оригинал.
 */
static int window_setting_id(int type)
{
	if (ain->version < 14)
		return type;
	switch (type) {
	case 0: return WINDOW_SETTINGS_ASPECT_RATIO;
	case 1: return WINDOW_SETTINGS_SCALING_TYPE;
	case 2: return WINDOW_SETTINGS_MINIMIZE_BY_FULL_SCREEN_INACTIVE;
	case 3: return WINDOW_SETTINGS_BACK_TO_TITLE_CONFIRM;
	case 4: return WINDOW_SETTINGS_CLOSE_GAME_CONFIRM;
	default: return -1;
	}
}

static void save_window_settings(void)
{
	cJSON *root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "wait_vsync", window_settings.wait_vsync);
	save_json("WindowSetting.json", root);
	cJSON_Delete(root);
}

static void load_window_settings(void)
{
	cJSON *root = load_json("WindowSetting.json");
	if (!root)
		return;
	cJSON *v;
	if ((v = cJSON_GetObjectItem(root, "wait_vsync"))) {
		window_settings.wait_vsync = cJSON_IsTrue(v);
		gfx_set_wait_vsync(window_settings.wait_vsync);
	}
	cJSON_Delete(root);
}

static bool SystemService_SetWindowSetting(int type, int value)
{
	switch (window_setting_id(type)) {
	case WINDOW_SETTINGS_ASPECT_RATIO:
		window_settings.aspect_ratio = value;
		break;
	case WINDOW_SETTINGS_SCALING_TYPE:
		window_settings.scaling_type = value;
		break;
	case WINDOW_SETTINGS_WAIT_VSYNC:
		window_settings.wait_vsync = value;
		gfx_set_wait_vsync(value);
		break;
	case WINDOW_SETTINGS_RECORD_POS_SIZE:
		window_settings.record_pos_size = value;
		break;
	case WINDOW_SETTINGS_MINIMIZE_BY_FULL_SCREEN_INACTIVE:
		window_settings.minimize_by_full_screen_inactive = value;
		break;
	case WINDOW_SETTINGS_BACK_TO_TITLE_CONFIRM:
		window_settings.back_to_title_confirm = value;
		break;
	case WINDOW_SETTINGS_CLOSE_GAME_CONFIRM:
		window_settings.close_game_confirm = value;
		break;
	default:
		WARNING("Invalid window setting type: %d", type);
		return false;
	}
	save_window_settings();
	return true;
}

static bool SystemService_GetWindowSetting(int type, int *value)
{
	switch (window_setting_id(type)) {
	case WINDOW_SETTINGS_ASPECT_RATIO:
		*value = window_settings.aspect_ratio;
		break;
	case WINDOW_SETTINGS_SCALING_TYPE:
		*value = window_settings.scaling_type;
		break;
	case WINDOW_SETTINGS_WAIT_VSYNC:
		*value = window_settings.wait_vsync;
		break;
	case WINDOW_SETTINGS_RECORD_POS_SIZE:
		*value = window_settings.record_pos_size;
		break;
	case WINDOW_SETTINGS_MINIMIZE_BY_FULL_SCREEN_INACTIVE:
		*value = window_settings.minimize_by_full_screen_inactive;
		break;
	case WINDOW_SETTINGS_BACK_TO_TITLE_CONFIRM:
		*value = window_settings.back_to_title_confirm;
		break;
	case WINDOW_SETTINGS_CLOSE_GAME_CONFIRM:
		*value = window_settings.close_game_confirm;
		break;
	default:
		WARNING("Invalid window setting type: %d", type);
		return false;
	}
	return true;
}

// XXX: Values for 'type' above 1 are invalid in Haru Urare, may be different in other games
#define NR_MOUSE_CURSOR_CONFIG 1
static int mouse_cursor_config[NR_MOUSE_CURSOR_CONFIG] = {0};

static bool SystemService_SetMouseCursorConfig(int type, int value)
{
	if (type < 0 || type >= NR_MOUSE_CURSOR_CONFIG) {
		WARNING("Invalid mouse cursor config type: %d", type);
		return false;
	}
	mouse_cursor_config[type] = value;
	return true;
}

static bool SystemService_GetMouseCursorConfig(int type, int *value)
{
	if (type < 0 || type >= NR_MOUSE_CURSOR_CONFIG) {
		WARNING("Invalid mouse cursor config type: %d", type);
		return false;
	}
	// XXX: the value returned here is always 1 or 0
	*value = !!mouse_cursor_config[type];
	return true;
}

//bool SystemService_RunProgram(struct string *program_file_name, struct string *parameter);
//bool SystemService_IsOpenedMutex(struct string *mutex_name);

void SystemService_GetGameFolderPath(struct string **folder_path)
{
	char *sjis = utf2sjis(config.game_dir, 0);
	*folder_path = make_string(sjis, strlen(sjis));
	free(sjis);
}

// Ixseal-форма (см. SystemService_GetGameName_ix).
static struct string *SystemService_GetGameFolderPath_ix(void)
{
	char *sjis = utf2sjis(config.game_dir, 0);
	struct string *s = make_string(sjis, strlen(sjis));
	free(sjis);
	return s;
}

static void SystemService_GetTime(int *hour, int *min, int *sec)
{
	int ms;
	get_time(hour, min, sec, &ms);
}

static bool SystemService_IsResetOnce(void)
{
	return vm_reset_once;
}

static bool SystemService_IsResetOnce_Drapeko(struct string **text)
{
	*text = cstr_to_string("XXX TTT YYY"); // ???
	return vm_reset_once;
}

static char *get_manual_filename(void) {
	char *manual_path = path_join("Manual", "index.html");
	char *file_path = path_join(config.game_dir, manual_path);
	free(manual_path);
	return file_path;
}

static bool SystemService_IsExistPlayingManual(void) {
	char *filename = get_manual_filename();
	const bool exists = file_exists(filename);
	free(filename);
	return exists;
}

#ifndef _WIN32
static char *percent_encode(const char *str) {
	const char *hex = "0123456789ABCDEF";
	// Worst case all characters are percent-encoded
	char *encoded = xmalloc(strlen(str) * 3 + 1);

	char *p = encoded;
	while (*str) {
		const unsigned char c = *str++;
		if ((c >= 'a' && c <= 'z') ||
			(c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
			*p++ = (char)c;
		} else {
			*p++ = '%';
			*p++ = hex[c >> 4];
			*p++ = hex[c & 15];
		}
	}

	*p = '\0';
	return encoded;
}
#endif

static void SystemService_OpenPlayingManual(void) {
	if (!SystemService_IsExistPlayingManual()) {
		return;
	}
#ifdef __ANDROID__
	const int COMMAND_OPEN_PLAYING_MANUAL = 0x8000;
	SDL_AndroidSendMessage(COMMAND_OPEN_PLAYING_MANUAL, 0);
#else
	char *filename = get_manual_filename();

	char *real_path = realpath_utf8(filename);
	free(filename);
	if (!real_path) {
		return;
	}

#ifdef _WIN32
	const char *prefix = "file:///";
	char *path_component = real_path;
#else
	const char *prefix = "file://";
	char *path_component = percent_encode(real_path);
	free(real_path);
#endif
	char *url = xmalloc(strlen(prefix) + strlen(path_component) + 1);
	strcpy(url, prefix);
	strcat(url, path_component);

	if (SDL_OpenURL(url) < 0) {
		WARNING("Failed to open manual at '%s': %s", url, SDL_GetError());
	}

	free(url);
	free(path_component);
#endif
}

/*
 * ★ОЧЕРЕДЬ СИСТЕМНЫХ СООБЩЕНИЙ РАНТАЙМА.
 *
 * Игра разбирает её в `_system::detail::ProcessSystemMessage` (Dohna, FUNC 20605):
 * `if (!IsExistSystemMessage()) return; if (!PopSystemMessage(&n)) error;` и
 * раскладывает номер по `switch`:
 *   1 — `gamesave::detail::SystemSuspend`: сохранить игру в служебный слот −6
 *       («продолжить с того же места»); при старте `main` читает его обратно
 *       (`IsSystemSuspended` → `SystemResume`);
 *   2 — `DeleteSuspendSaveFile` — этот слот удалить;
 *   3 — открыть конфиг (`Ｓ＿コンフィグ`);
 *   4 — `PrepareReload`: слот −7 перед пересозданием окна (`IsReloaded` → `Reload`);
 *   5 — снимок экрана в файл; номер приходит ВМЕСТЕ СО СТРОКОЙ, её игра забирает
 *       вторым вызовом `PopSystemMessageString`.
 *
 * Пока очередь отвечала «пусто», suspend не писался НИКОГДА: закрытие окна убивало
 * процесс на месте (`SDL_QUIT → vm_exit`). Тем самым у нас отсутствовало не только
 * «продолжить с того же места», но и ЕДИНСТВЕННЫЙ путь, на котором игра читает
 * образ частей обратно: `parts::detail::Load` вызывается лишь из
 * `gamesave::detail::セーブ実行_ロード後復帰処理`, куда приходит `partsLibraryData`.
 */
enum system_message {
	SYS_MSG_SUSPEND = 1,
	SYS_MSG_DELETE_SUSPEND = 2,
	SYS_MSG_OPEN_CONFIG = 3,
	SYS_MSG_PREPARE_RELOAD = 4,
	SYS_MSG_SNAPSHOT = 5,
};

#define SYS_MSG_QUEUE_MAX 8
static int sys_msg_queue[SYS_MSG_QUEUE_MAX];
static struct string *sys_msg_param[SYS_MSG_QUEUE_MAX];
static int sys_msg_head, sys_msg_count;
// Строка последнего снятого сообщения: игра забирает её отдельным вызовом.
static struct string *sys_msg_last_param;
// Закрытие окна: сообщение поставлено — ждём, пока игра его переварит, и уходим.
static bool sys_msg_quit_requested, sys_msg_quit_taken;
static uint32_t sys_msg_quit_ticks;
// Сколько ждём игру, прежде чем закрыться самим (см. sysservice_quit_watchdog).
#define SYS_MSG_QUIT_GRACE_MS 1500

void sysservice_push_system_message(int message, struct string *param)
{
	if (sys_msg_count >= SYS_MSG_QUEUE_MAX) {
		WARNING("очередь системных сообщений переполнена, теряю %d", message);
		return;
	}
	int slot = (sys_msg_head + sys_msg_count) % SYS_MSG_QUEUE_MAX;
	sys_msg_queue[slot] = message;
	sys_msg_param[slot] = param ? string_ref(param) : NULL;
	sys_msg_count++;
}

/*
 * Запрос закрытия окна (крестик, SIGTERM/SIGINT через SDL). Windows-рантайм на
 * WM_CLOSE даёт игре сообщение 1 и завершает приложение уже ПОСЛЕ того, как та
 * записала suspend-сейв, — повторяем ровно это. Второй запрос (пользователь жмёт
 * крестик снова, потому что окно не закрылось) уходит немедленно: значит игра
 * сообщение не разбирает и ждать нечего.
 */
/*
 * ★SUSPEND ПРИ ВЫХОДЕ — ТОЛЬКО ПОД ФЛАГОМ `XSYS4_SUSPEND_ON_QUIT=1`.
 *
 * Оригинал состояние после обычного выхода не возвращает: закрыл окно — при
 * следующем запуске титул с начала. Значит слот −6 у него служит чему-то
 * другому (отладка, перезапуск окна), и писать его на каждый крестик — наша
 * самодеятельность.
 *
 * Цена самодеятельности замерена: игра читает `SystemSuspend.asd` на КАЖДОМ
 * старте (`System.ResumeLoad`, видно в XSYS4_SAVE_TRACE), и после нашего
 * восстановления экран выходит НЕМЫМ — части на месте и курсор над пунктом
 * опознаётся (`hovered=1`), но НИ ОДНОЙ части не помечено кликабельной
 * (`clk=1`: при обычном старте таких 46, после resume — ноль). То есть каждый
 * второй запуск игры оказывался нерабочим, и лечилось это только удалением
 * файла руками.
 */
static bool suspend_on_quit_enabled(void)
{
	static int on = -1;
	if (on < 0) {
		const char *v = getenv("XSYS4_SUSPEND_ON_QUIT");
		on = v && *v && strcmp(v, "0") ? 1 : 0;
	}
	return on;
}

/*
 * Снимок состояния по F8: то же системное сообщение 1, но БЕЗ пометки выхода —
 * игра запишет образ и продолжит играть. Восстановление из такого образа
 * возвращает её в игровой цикл, а не в ветку завершения; оригинал этот слот
 * использует именно так (снимок карты, `SceneMap@SaveMapSnapShot`).
 */
void sysservice_request_snapshot(void)
{
	NOTICE("SUSPEND: снимок состояния по F8 (слот −6)");
	sysservice_push_system_message(SYS_MSG_SUSPEND, NULL);
}

void sysservice_request_quit(void)
{
	if (!suspend_on_quit_enabled())
		vm_exit(0);
	if (sys_msg_quit_requested)
		vm_exit(0);
	sysservice_push_system_message(SYS_MSG_SUSPEND, NULL);
	sys_msg_quit_requested = true;
	sys_msg_quit_ticks = SDL_GetTicks();
}

/*
 * Страховка на игры, которые системные сообщения не разбирают вовсе (Tsumamigui,
 * Escalayer, Haha Ranman — `ProcessSystemMessage` есть только у Ixseal-набора):
 * там сообщение никто не заберёт, и без этого крестик перестал бы закрывать окно.
 * Ждём короткую паузу и уходим сами — тем же путём, что до правки.
 */
void sysservice_quit_watchdog(void)
{
	if (!sys_msg_quit_requested || sys_msg_quit_taken)
		return;
	if (SDL_GetTicks() - sys_msg_quit_ticks > SYS_MSG_QUIT_GRACE_MS)
		vm_exit(0);
}

static bool SystemService_IsExistSystemMessage(void)
{
	if (sys_msg_count > 0)
		return true;
	/*
	 * Очередь опустела. Если это был наш запрос выхода, suspend-сейв уже НА ДИСКЕ:
	 * `SystemSuspend` игра вызывает синхронно внутри того же `ProcessSystemMessage`,
	 * а сюда мы попадаем на следующем витке игрового цикла.
	 */
	if (sys_msg_quit_requested && sys_msg_quit_taken)
		vm_exit(0);
	return false;
}

static bool SystemService_PopSystemMessage(int *message)
{
	// ref-выходы обязаны быть заполнены даже при отказе (см. §7 FINDINGS).
	if (message)
		*message = 0;
	if (!sys_msg_count)
		return false;

	int msg = sys_msg_queue[sys_msg_head];
	if (sys_msg_last_param)
		free_string(sys_msg_last_param);
	sys_msg_last_param = sys_msg_param[sys_msg_head];
	sys_msg_param[sys_msg_head] = NULL;
	sys_msg_head = (sys_msg_head + 1) % SYS_MSG_QUEUE_MAX;
	sys_msg_count--;

	if (msg == SYS_MSG_SUSPEND && sys_msg_quit_requested)
		sys_msg_quit_taken = true;
	if (message)
		*message = msg;
	return true;
}

// Строковый параметр сообщения (сейчас только 5 — имя файла снимка экрана).
static bool SystemService_PopSystemMessageString(struct string **out)
{
	if (!sys_msg_last_param)
		return false;
	if (out) {
		if (*out)
			free_string(*out);
		*out = string_ref(sys_msg_last_param);
	}
	free_string(sys_msg_last_param);
	sys_msg_last_param = NULL;
	return true;
}

static void SystemService_RestrainScreensaver(void) { }

/*
 * Индикатор «подождите» рантайма (`_system::detail::BeginWaitMessage` даёт 1,
 * `EndWaitMessage` — 0). Своего индикатора у движка нет, а прочитать состояние
 * игра не может — геттера у функции нет во всей библиотеке, — так что для
 * игрового кода no-op неотличим от настоящего показа.
 */
static void SystemService_ShowWaitMessage(bool show)
{
	(void)show;
}

/*
 * ★★СОСТОЯНИЕ БИБЛИОТЕКИ В БУФЕР (Ixseal v14): `bool Save(wrap<array<int>>)` /
 * `bool Load(wrap<array<int>>)`. Это ОБЫЧНАЯ сериализация того, чем владеет
 * SystemService, а НЕ снимок VM: снимок делает сама игра рядом, через
 * `system.ResumeSave` (см. src/resume.c).
 *
 * Доказательство порядком в `gamesave::detail::セーブ実行` (Dohna):
 *   backlog → `MainEXFile.Save` → `parts::detail::Save` → `SystemService.Save`
 *   → `HashMap.Save` → … → `system.ResumeSave`,
 * и у каждого из первых пяти вызовов сразу за ним стоит только проверка на
 * ошибку (`IFZ` → `system.Error("Failed to save …")`). Возобновление приходит
 * ИМЕННО в точку `ResumeSave`, и дальше по коду идёт
 * `セーブ実行_ロード後復帰処理`, где буферы читаются обратно в порядке
 * `MainEXFile.Load` → … → `SystemService.Load` → `parts::detail::Load`.
 *
 * Отсюда: `Load` ОБЯЗАН вернуть управление. Прежняя реализация носила в буфере
 * готовый RSM-образ и уходила из `Load` прыжком в точку `Save` — тогда
 * `parts::detail::Load` уже не исполнялся: образ частей не восстанавливался
 * (`XPE load` не случался ни разу), возобновление в данже падало на
 * `SceneHitokari@MoveGrid` (`Out of bounds heap index: -1/0`), а возврат «0»
 * в точку Save игра читала как отказ — `*GAME ERROR*: Failed to save system
 * service.` ★Прежнюю модель оставили под ручкой `XSYS4_SS_SAVE_RESUME=1`:
 * на ней держался вход в hunting-фазу (снимок карты `SceneMap@SaveMapSnapShot`
 * → `SnapShotSave@Save`), и A/B на одном бинаре показывает разницу сразу.
 *
 * Формат буфера наш: игра его только хранит и отдаёт обратно. Носим то, что
 * НЕ переезжает вместе с VM-образом, потому что живёт в C: словари
 * SystemVariable/GameVariable и настройку курсора. Масштаб вида и режим окна
 * сюда не идут намеренно — их игра задаёт заново при старте через `AFL_View_*`
 * (и хранит в своём конфиге, `コンフィグセーブ` рядом в том же `セーブ実行`).
 */
#define SS_RESUME_MAGIC 0x53535253  // "SRSS" — прежний формат (RSM-образ)
#define SS_STATE_MAGIC 0x53535453   // "SSTS" — состояние библиотеки
#define SS_STATE_VERSION 1
#define SS_RESUME_TMP "SystemServiceResume.rsm"

static void sv_store_write(struct iarray_writer *w, struct sv_store *st)
{
	iarray_write(w, st->nr_entries);
	for (int i = 0; i < st->nr_entries; i++) {
		iarray_write_string(w, st->entries[i].key);
		iarray_write_string(w, st->entries[i].value);
	}
}

static void sv_store_read(struct iarray_reader *r, struct sv_store *st)
{
	// Порядок ключей — порядок вставки (его видит NumofKey/GetKey), поэтому
	// читаем в тот же словарь через sv_set, а прежнее содержимое сносим.
	for (int i = 0; i < st->nr_entries; i++) {
		free_string(st->entries[i].key);
		free_string(st->entries[i].value);
	}
	st->nr_entries = 0;

	int n = iarray_read(r);
	if (n < 0 || n > 100000) {
		WARNING("SystemService.Load: битое число переменных (%d)", n);
		return;
	}
	for (int i = 0; i < n; i++) {
		struct string *k = iarray_read_string(r);
		struct string *v = iarray_read_string(r);
		sv_set(st, k, v);
		free_string(k);
		free_string(v);
	}
}

static bool ss_save_resume_image(struct page **buffer)
{
	char *path = savedir_path(SS_RESUME_TMP);
	if (getenv("XSYS4_SAVE_TRACE"))
		NOTICE("SAVETRACE SystemService.Save -> '%s'", path);
	int vm_save_image(const char *key, const char *path, bool hll_convention);
	int r = vm_save_image("SystemService", path, true);
	if (!r) {
		free(path);
		return false;
	}
	size_t len = 0;
	uint8_t *data = file_read(path, &len);
	unlink(path);
	free(path);
	if (!data) {
		WARNING("SystemService.Save: образ записался, но не прочитался");
		return false;
	}
	struct iarray_writer w;
	iarray_init_writer(&w, NULL);
	iarray_write(&w, SS_RESUME_MAGIC);
	iarray_write(&w, (int)len);
	for (size_t i = 0; i < len; i += 4) {
		int32_t v = 0;
		memcpy(&v, data + i, len - i >= 4 ? 4 : len - i);
		iarray_write(&w, v);
	}
	free(data);
	if (*buffer) {
		delete_page_vars(*buffer);
		free_page(*buffer);
	}
	*buffer = iarray_to_page(&w);
	iarray_free_writer(&w);
	return true;
}

static bool ss_load_resume_image(struct page **buffer)
{
	struct iarray_reader r;
	iarray_init_reader(&r, *buffer, NULL);
	if (iarray_read(&r) != SS_RESUME_MAGIC) {
		WARNING("SystemService.Load: буфер не похож на RSM-образ");
		return false;
	}
	int len = iarray_read(&r);
	if (len <= 0 || (unsigned)(*buffer)->nr_vars < 2 + ((unsigned)len + 3) / 4) {
		WARNING("SystemService.Load: битая длина %d при %d int", len, (*buffer)->nr_vars);
		return false;
	}
	uint8_t *data = xmalloc(len);
	for (int i = 0; i < len; i += 4) {
		int32_t v = iarray_read(&r);
		memcpy(data + i, &v, len - i >= 4 ? 4 : len - i);
	}
	char *path = savedir_path(SS_RESUME_TMP);
	if (getenv("XSYS4_SAVE_TRACE"))
		NOTICE("SAVETRACE SystemService.Load <- '%s' (%d байт)", path, len);
	bool ok = file_write(path, data, len);
	free(data);
	if (!ok) {
		free(path);
		WARNING("SystemService.Load: не записался временный образ");
		return false;
	}
	void vm_load_image(const char *key, const char *path);
	vm_load_image("SystemService", path);
	// При успехе сюда не возвращаемся: выполнение ушло в точку Save.
	unlink(path);
	free(path);
	return false;
}

// Прежняя модель (RSM-образ вместо состояния) — только для A/B-замеров.
static bool ss_save_resume_mode(void)
{
	static int on = -1;
	if (on < 0)
		on = getenv("XSYS4_SS_SAVE_RESUME") ? 1 : 0;
	return on;
}

static bool SystemService_Save(struct page **buffer)
{
	if (ss_save_resume_mode())
		return ss_save_resume_image(buffer);

	struct iarray_writer w;
	iarray_init_writer(&w, NULL);
	iarray_write(&w, SS_STATE_MAGIC);
	iarray_write(&w, SS_STATE_VERSION);
	sv_store_write(&w, &sv_system);
	sv_store_write(&w, &sv_game);
	iarray_write(&w, NR_MOUSE_CURSOR_CONFIG);
	for (int i = 0; i < NR_MOUSE_CURSOR_CONFIG; i++)
		iarray_write(&w, mouse_cursor_config[i]);

	if (*buffer) {
		delete_page_vars(*buffer);
		free_page(*buffer);
	}
	*buffer = iarray_to_page(&w);
	iarray_free_writer(&w);
	if (getenv("XSYS4_SAVE_TRACE"))
		NOTICE("SAVETRACE SystemService.Save: sys=%d game=%d переменных",
		       sv_system.nr_entries, sv_game.nr_entries);
	return true;
}

static bool SystemService_Load(struct page **buffer)
{
	if (!*buffer || (*buffer)->nr_vars < 2) {
		WARNING("SystemService.Load: пустой буфер");
		return false;
	}
	// Образы прежней сборки (RSM) читаем прежним путём: у них своя магия, и без
	// этого возобновление старого сейва молча получило бы пустое состояние.
	struct iarray_reader probe;
	iarray_init_reader(&probe, *buffer, NULL);
	int magic = iarray_read(&probe);
	if (magic == SS_RESUME_MAGIC)
		return ss_load_resume_image(buffer);
	if (magic != SS_STATE_MAGIC) {
		WARNING("SystemService.Load: чужой буфер (магия 0x%08x)", magic);
		return false;
	}

	struct iarray_reader r;
	iarray_init_reader(&r, *buffer, NULL);
	iarray_read(&r); // магия
	int version = iarray_read(&r);
	if (version > SS_STATE_VERSION) {
		WARNING("SystemService.Load: версия состояния %d новее нашей", version);
		return false;
	}
	sv_store_read(&r, &sv_system);
	sv_store_read(&r, &sv_game);
	int nr_cursor = iarray_read(&r);
	for (int i = 0; i < nr_cursor; i++) {
		int v = iarray_read(&r);
		if (i < NR_MOUSE_CURSOR_CONFIG)
			mouse_cursor_config[i] = v;
	}
	if (getenv("XSYS4_SAVE_TRACE"))
		NOTICE("SAVETRACE SystemService.Load: sys=%d game=%d переменных",
		       sv_system.nr_entries, sv_game.nr_entries);
	return true;
}

/*
 * Резервные копии сейвов (Ixseal). Игра лишь РЕГИСТРИРУЕТ имена файлов
 * (`AddBackupSaveFileName`) и просит скопировать их (`BackupSaveFile`) — ни
 * геттера, ни возвращаемого значения у этой пары нет, так что имена копий и
 * восстановление из них целиком внутреннее дело движка.
 *
 * Сайты: `_system::detail::Init` (fno 20591) регистрирует имена вроде
 * `AFCGMode.asd` и значения EX-ключей «重要セーブファイル名N», а
 * `BackupSaveFile` вызывается один раз — при возврате в титул
 * (`・タイトルに戻る・確認なし`, fno 20526).
 */
#define MAX_BACKUP_SAVE_FILES 32
static struct string *backup_save_names[MAX_BACKUP_SAVE_FILES];
static int nr_backup_save_names = 0;

static void SystemService_AddBackupSaveFileName(struct string *name)
{
	if (!name || !name->size)
		return;
	for (int i = 0; i < nr_backup_save_names; i++) {
		if (!strcmp(backup_save_names[i]->text, name->text))
			return;
	}
	if (nr_backup_save_names == MAX_BACKUP_SAVE_FILES) {
		WARNING("SystemService.AddBackupSaveFileName: список полон (%d), «%s» пропущено",
			MAX_BACKUP_SAVE_FILES, display_sjis0(name->text));
		return;
	}
	backup_save_names[nr_backup_save_names++] = string_ref(name);
}

static bool copy_file_bytes(const char *src, const char *dst)
{
	size_t len;
	void *data = file_read(src, &len);
	if (!data)
		return false;  // файла может не быть — это норма
	bool ok = file_write(dst, data, len);
	free(data);
	return ok;
}

static void SystemService_BackupSaveFile(void)
{
	// Суффикс копии по байткоду не установлен (игра его никогда не читает),
	// поэтому берём общепринятый `.bak` рядом с оригиналом в папке сейвов.
	for (int i = 0; i < nr_backup_save_names; i++) {
		char *src = savedir_path(backup_save_names[i]->text);
		char *dst = xmalloc(strlen(src) + 5);
		sprintf(dst, "%s.bak", src);
		if (!copy_file_bytes(src, dst))
			NOTICE("SystemService.BackupSaveFile: нет %s — пропущено", src);
		free(dst);
		free(src);
	}
}

/*
 * Сведения о машине для экрана «Questionnaire» (アンケート) — той самой анкеты,
 * которую игра предлагает отправить по сети. Четыре геттера ниже плюс семь
 * функций `HTTPDownloader` — единственные СТРОГО достижимые дыры Tsumamigui
 * (FINDINGS §5y), и попадают в них по одному пути: пользователь соглашается
 * на «подключиться к сети?».
 *
 * Оригинал брал их из WinAPI (`GlobalMemoryStatus`, CPUID, `GetVersionEx`).
 * Здесь — настоящие значения Linux/Android из `/proc`, а не выдуманные:
 * анкета их только ПОКАЗЫВАЕТ и отправляет, поэтому враньё было бы видно
 * пользователю, а неверный формат — нет.
 */
static char *proc_field(const char *path, const char *key)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char line[512];
	size_t keylen = strlen(key);
	char *out = NULL;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, keylen))
			continue;
		char *colon = strchr(line, ':');
		if (!colon)
			continue;
		char *v = colon + 1;
		while (*v == ' ' || *v == '\t')
			v++;
		char *end = v + strlen(v);
		while (end > v && (end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t'))
			end--;
		*end = '\0';
		out = xstrdup(v);
		break;
	}
	fclose(f);
	return out;
}

static void set_out_string(struct string **dst, const char *text)
{
	if (!dst)
		return;
	if (*dst)
		free_string(*dst);
	*dst = cstr_to_string(text ? text : "");
}

static void SystemService_GetCPUInfo(struct string **vendor, int *signature,
				     int *cpu_flag, struct string **brand)
{
	char *v = proc_field("/proc/cpuinfo", "vendor_id");
	char *b = proc_field("/proc/cpuinfo", "model name");
	if (!b)
		b = proc_field("/proc/cpuinfo", "Processor");   // ARM/Android
	set_out_string(vendor, v ? v : "unknown");
	set_out_string(brand, b ? b : "unknown");
	free(v);
	free(b);
	// CPUID-signature и битовая маска расширений: своего CPUID у нас нет, а
	// выдуманные биты игра могла бы показать в анкете как правду. Ноль честнее.
	if (signature)
		*signature = 0;
	if (cpu_flag)
		*cpu_flag = 0;
}

static void SystemService_GetMemoryInfo(int *max, int *use)
{
	long total_kb = 0, avail_kb = 0;
	char *t = proc_field("/proc/meminfo", "MemTotal");
	char *a = proc_field("/proc/meminfo", "MemAvailable");
	if (t)
		total_kb = strtol(t, NULL, 10);
	if (a)
		avail_kb = strtol(a, NULL, 10);
	free(t);
	free(a);
	// В БАЙТАХ, как `MEMORYSTATUS.dwTotalPhys` у оригинала. Значения int, и на
	// машине с >2 ГБ они переполнились бы — Windows-версия в такой же ситуации
	// сама возвращала мусор, поэтому насыщаем по INT_MAX, а не заворачиваем.
	long long total_b = (long long)total_kb * 1024;
	long long used_b = (long long)(total_kb - avail_kb) * 1024;
	if (max)
		*max = total_b > INT_MAX ? INT_MAX : (int)total_b;
	if (use)
		*use = used_b > INT_MAX ? INT_MAX : (used_b < 0 ? 0 : (int)used_b);
}

static void SystemService_GetOSInfo(struct string **text)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "%s (xsystem4)", SDL_GetPlatform());
	set_out_string(text, buf);
}

static void SystemService_GetScreenInfo(struct string **text)
{
	char buf[128];
	SDL_DisplayMode mode;
	if (SDL_GetCurrentDisplayMode(0, &mode) == 0) {
		snprintf(buf, sizeof(buf), "%dx%d %dbpp %dHz", mode.w, mode.h,
			 SDL_BITSPERPIXEL(mode.format), mode.refresh_rate);
	} else {
		snprintf(buf, sizeof(buf), "unknown");
	}
	set_out_string(text, buf);
}

/*
 * Видеопамять: ни SDL, ни GL-ES её переносимо не отдают (расширения
 * `NVX_gpu_memory_info`/`ATI_meminfo` вендорные и на Android отсутствуют).
 * Ноль = «не знаю»; выдуманный объём в анкете был бы ложью.
 */
static int SystemService_Debug_GetUseVideoMemorySize(void)
{
	return 0;
}

// Rance 01
static void SystemService_Rance0123456789(struct string **text)
{
	*text = cstr_to_string("-RANCE010ECNAR-");
}

// Rance 01 trial edition
static void SystemService_XXXXX01XXXXXXXX(struct string **text)
{
	*text = cstr_to_string("RANCE01RANCEKAKKOII");
}

// Drapeko
static void SystemService_Test(struct string **text)
{
	*text = cstr_to_string("DELETE ALL 758490275489207548093");
}

// Drapeko trial edition
static void SystemService_DRPKT(struct string **text)
{
	*text = cstr_to_string("DRPKT QWERTY NUFUAUEO 75849027582754829");
}

// Rance 9
static void SystemService_Rance96161988(struct string **text) {
	*text = cstr_to_string("=Rance99/RANCE99=");
}

// Pascha3 Plus Contents
static void SystemService_XXX(struct string **text) {
	*text = cstr_to_string("FORMAT HDD ERASE 578205024758284076520478254092784789752384758204687293");
}

/*
 * Масштаб игрового вида внутри окна. Пара сеттер/геттер (`SetGameViewScaleRate`
 * fn11 / `GetGameViewScaleRate` fn12), у v6/v7 её нет вовсе (проверено
 * ainliball по трём .ain), обёртки игры — прямые проходные
 * (`view::detail::SetGameViewScaleRate` @0x4ae814,
 * `AFL_View_GetGameViewScaleRate` @0x4ae098).
 *
 * Зачем геттер игре: `AFL_View_CalcGameViewPos` (@0x4ae0ea) пересчитывает
 * координату из оконной в видовую и при rate == 1.0f возвращает её КАК ЕСТЬ,
 * иначе делит. Через него идёт `input::detail::GetMousePos`, то есть без
 * геттера вставал каждый кадр титула (`TitleCharacterView@UpdateCharacterPos`).
 *
 * Движок рисует вид 1:1 в окно того же размера, поэтому честное значение — 1.0,
 * и пересчёт мыши становится тождественным. Само масштабирование вида не
 * реализовано: если игра выставит не 1.0, координаты разойдутся — вместо тихого
 * дефолта стоит проверка допущения.
 */
static float sys_game_view_scale_rate = 1.0f;

static void SystemService_SetGameViewScaleRate(float rate)
{
	if (rate != 1.0f) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("SetGameViewScaleRate(%f): масштабирование игрового вида "
				"не реализовано, координаты мыши не пересчитываются", rate);
		}
	}
	sys_game_view_scale_rate = rate;
}

static float SystemService_GetGameViewScaleRate(void)
{
	return sys_game_view_scale_rate;
}

static void SystemService_PreLink(void);

static void SystemService_ModuleInit(void)
{
	load_window_settings();
}

HLL_LIBRARY(SystemService,
	    HLL_EXPORT(_PreLink, SystemService_PreLink),
	    HLL_EXPORT(_ModuleInit, SystemService_ModuleInit),
	    HLL_EXPORT(GetMixerNumof, mixer_get_numof),
	    HLL_EXPORT(GetMixerName, SystemService_GetMixerName),
	    HLL_EXPORT(GetMixerVolume, mixer_get_volume),
	    HLL_EXPORT(GetMixerDefaultVolume, SystemService_GetMixerDefaultVolume),
	    HLL_EXPORT(GetMixerMute, mixer_get_mute),
	    HLL_EXPORT(SetMixerName, SystemService_SetMixerName),
	    HLL_EXPORT(SetMixerVolume, mixer_set_volume),
	    HLL_EXPORT(SetMixerMute, mixer_set_mute),
	    HLL_EXPORT(GetGameVersion, SystemService_GetGameVersion),
	    HLL_EXPORT(GetPlatformType, SystemService_GetPlatformType),
	    HLL_EXPORT(SetHookCloseApp, SystemService_SetHookCloseApp),
	    HLL_EXPORT(IsHookCloseApp, SystemService_IsHookCloseApp),
	    HLL_EXPORT(GetGameVersionByText, SystemService_GetGameVersionByText),
	    HLL_EXPORT(GetGameName, SystemService_GetGameName),
	    HLL_EXPORT(AddURLMenu, SystemService_AddURLMenu),
	    HLL_EXPORT(EscalayerReboot011116, SystemService_EscalayerReboot011116),
	    HLL_EXPORT(SetAndroidViewKeepScreen, SystemService_SetAndroidViewKeepScreen),
	    HLL_EXPORT(SetAndroidViewOrientation, SystemService_SetAndroidViewOrientation),
	    HLL_EXPORT(SetViewResizableMode, SystemService_SetViewResizableMode),
	    HLL_EXPORT(IsViewResizableMode, SystemService_IsViewResizableMode),
	    HLL_EXPORT(SetWaitVSyncMode, SystemService_SetWaitVSyncMode),
	    HLL_EXPORT(GetWaitVSyncMode, SystemService_GetWaitVSyncMode),
	    HLL_EXPORT(GetUsePower2Texture, SystemService_GetUsePower2Texture),
	    HLL_EXPORT(GetAntiAliasingMode, SystemService_GetAntiAliasingMode),
	    HLL_EXPORT(IsFullScreen, SystemService_IsFullScreen),
	    HLL_EXPORT(ChangeNormalScreen, SystemService_ChangeNormalScreen),
	    HLL_EXPORT(ChangeFullScreen, SystemService_ChangeFullScreen),
	    HLL_EXPORT(InitMainWindowPosAndSize, SystemService_InitMainWindowPosAndSize),
	    HLL_EXPORT(UpdateView, SystemService_UpdateView),
	    HLL_EXPORT(SetGameViewScaleRate, SystemService_SetGameViewScaleRate),
	    HLL_EXPORT(GetGameViewScaleRate, SystemService_GetGameViewScaleRate),
	    HLL_EXPORT(GetViewWidth, SystemService_GetViewWidth),
	    HLL_EXPORT(GetViewHeight, SystemService_GetViewHeight),
	    HLL_EXPORT(GetDefaultViewWidth, SystemService_GetDefaultViewWidth),
	    HLL_EXPORT(GetDefaultViewHeight, SystemService_GetDefaultViewHeight),
	    HLL_EXPORT(MoveMouseCursorPosImmediately, SystemService_MoveMouseCursorPosImmediately),
	    HLL_EXPORT(SetHideMouseCursorByGame, SystemService_SetHideMouseCursorByGame),
	    HLL_TODO_EXPORT(GetHideMouseCursorByGame, SystemService_GetHideMouseCursorByGame),
	    HLL_EXPORT(SetUsePower2Texture, SystemService_SetUsePower2Texture),
	    HLL_EXPORT(SetAntiAliasingMode, SystemService_SetAntiAliasingMode),
	    HLL_EXPORT(SetWindowSetting, SystemService_SetWindowSetting),
	    HLL_EXPORT(GetWindowSetting, SystemService_GetWindowSetting),
	    HLL_EXPORT(SetMouseCursorConfig, SystemService_SetMouseCursorConfig),
	    HLL_EXPORT(GetMouseCursorConfig, SystemService_GetMouseCursorConfig),
	    HLL_TODO_EXPORT(RunProgram, SystemService_RunProgram),
	    HLL_TODO_EXPORT(IsOpenedMutex, SystemService_IsOpenedMutex),
	    HLL_EXPORT(SystemVariable_IsExist, SystemService_SystemVariable_IsExist),
	    HLL_EXPORT(SystemVariable_Set, SystemService_SystemVariable_Set),
	    HLL_EXPORT(SystemVariable_Get, SystemService_SystemVariable_Get),
	    HLL_EXPORT(SystemVariable_NumofKey, SystemService_SystemVariable_NumofKey),
	    HLL_EXPORT(SystemVariable_GetKey, SystemService_SystemVariable_GetKey),
	    HLL_EXPORT(SystemVariable_Erase, SystemService_SystemVariable_Erase),
	    HLL_EXPORT(GameVariable_IsExist, SystemService_GameVariable_IsExist),
	    HLL_EXPORT(GameVariable_Set, SystemService_GameVariable_Set),
	    HLL_EXPORT(GameVariable_Get, SystemService_GameVariable_Get),
	    HLL_EXPORT(GameVariable_NumofKey, SystemService_GameVariable_NumofKey),
	    HLL_EXPORT(GameVariable_GetKey, SystemService_GameVariable_GetKey),
	    HLL_EXPORT(GameVariable_Erase, SystemService_GameVariable_Erase),
	    HLL_EXPORT(GetGameFolderPath, SystemService_GetGameFolderPath),
	    HLL_EXPORT(GetDate, get_date),
	    HLL_EXPORT(GetTime, SystemService_GetTime),
	    HLL_EXPORT(IsResetOnce, SystemService_IsResetOnce),
	    HLL_EXPORT(OpenPlayingManual, SystemService_OpenPlayingManual),
	    HLL_EXPORT(IsExistPlayingManual, SystemService_IsExistPlayingManual),
	    HLL_EXPORT(IsExistSystemMessage, SystemService_IsExistSystemMessage),
	    HLL_EXPORT(PopSystemMessage, SystemService_PopSystemMessage),
	    HLL_EXPORT(PopSystemMessageString, SystemService_PopSystemMessageString),
	    HLL_EXPORT(RestrainScreensaver, SystemService_RestrainScreensaver),
	    HLL_EXPORT(ShowWaitMessage, SystemService_ShowWaitMessage),
	    HLL_EXPORT(Save, SystemService_Save),
	    HLL_EXPORT(Load, SystemService_Load),
	    HLL_EXPORT(AddBackupSaveFileName, SystemService_AddBackupSaveFileName),
	    HLL_EXPORT(BackupSaveFile, SystemService_BackupSaveFile),
	    HLL_EXPORT(Debug_GetUseVideoMemorySize, SystemService_Debug_GetUseVideoMemorySize),
	    HLL_EXPORT(GetCPUInfo, SystemService_GetCPUInfo),
	    HLL_EXPORT(GetMemoryInfo, SystemService_GetMemoryInfo),
	    HLL_EXPORT(GetOSInfo, SystemService_GetOSInfo),
	    HLL_EXPORT(GetScreenInfo, SystemService_GetScreenInfo),
	    HLL_EXPORT(Rance0123456789, SystemService_Rance0123456789),
	    HLL_EXPORT(XXXXX01XXXXXXXX, SystemService_XXXXX01XXXXXXXX),
	    HLL_EXPORT(XXX, SystemService_XXX),
	    HLL_EXPORT(Test, SystemService_Test),
	    HLL_EXPORT(DRPKT, SystemService_DRPKT),
	    HLL_EXPORT(Rance96161988, SystemService_Rance96161988)
	);

static struct ain_hll_function *get_fun(int libno, const char *name)
{
	int fno = ain_get_library_function(ain, libno, name);
	return fno >= 0 ? &ain->libraries[libno].functions[fno] : NULL;
}

static void SystemService_PreLink(void)
{
	struct ain_hll_function *fun;
	int libno = ain_get_library(ain, "SystemService");
	assert(libno >= 0);

	fun = get_fun(libno, "IsResetOnce");
	if (fun && fun->nr_arguments == 1) {
		static_library_replace(&lib_SystemService, "IsResetOnce",
				SystemService_IsResetOnce_Drapeko);
	}

	// Ixseal отдаёт строку версии ВОЗВРАТОМ, а не через out-параметр.
	fun = get_fun(libno, "GetGameVersionByText");
	if (fun && fun->nr_arguments == 0) {
		static_library_replace(&lib_SystemService, "GetGameVersionByText",
				SystemService_GetGameVersionByText_ix);
	}

	// Тот же сдвиг формы у остальных строковых геттеров без аргументов.
	fun = get_fun(libno, "GetGameName");
	if (fun && fun->nr_arguments == 0) {
		static_library_replace(&lib_SystemService, "GetGameName",
				SystemService_GetGameName_ix);
	}
	fun = get_fun(libno, "GetGameFolderPath");
	if (fun && fun->nr_arguments == 0) {
		static_library_replace(&lib_SystemService, "GetGameFolderPath",
				SystemService_GetGameFolderPath_ix);
	}
}
