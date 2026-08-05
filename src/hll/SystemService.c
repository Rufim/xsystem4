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

#include "system4/ain.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "cJSON.h"
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
	if (n < 0 || (unsigned)n >= config.mixer_nr_channels)
		return false;
	*volume = config.mixer_volumes[n];
	return true;
}

static bool SystemService_SetMixerName(int n, struct string *name)
{
	return mixer_set_name(n, name->text);
}

static int SystemService_GetGameVersion(void)
{
	return ain->game_version;
}

static bool SystemService_GetGameVersionByText(struct string **text)
{
	// Анти-тамппер: игра сверяет строку версии рантайма System4 по фиксированным
	// позициям (Tsumamigui 3: [6:9]"]:7" + [13:16]"7d6" + [20:25]"6sadc" == её
	// строковая константа, плюс [5]==[32]). Иначе main крутит пустой цикл (чёрный
	// экран). Возвращаем строку-водяной знак, удовлетворяющую этой проверке.
	static const char watermark[] = "000000]:700007d600006sadc00000000";
	if (*text)
		free_string(*text);
	*text = cstr_to_string(watermark);
	return true;
}

static void SystemService_GetGameName(struct string **game_name)
{
	if (*game_name)
		free_string(*game_name);
	*game_name = cstr_to_string(config.game_name);
}

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
	switch (type) {
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
	switch (type) {
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

//static bool SystemService_IsExistSystemMessage(void);
HLL_QUIET_UNIMPLEMENTED(false, bool, SystemService, IsExistSystemMessage);
//static bool SystemService_PopSystemMessage(int *message);

static void SystemService_RestrainScreensaver(void) { }

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

//static int SystemService_Debug_GetUseVideoMemorySize(void);

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
	    HLL_EXPORT(GetViewWidth, SystemService_GetViewWidth),
	    HLL_EXPORT(GetViewHeight, SystemService_GetViewHeight),
	    HLL_EXPORT(GetDefaultViewWidth, SystemService_GetDefaultViewWidth),
	    HLL_EXPORT(GetDefaultViewHeight, SystemService_GetDefaultViewHeight),
	    HLL_EXPORT(MoveMouseCursorPosImmediately, SystemService_MoveMouseCursorPosImmediately),
	    HLL_EXPORT(SetHideMouseCursorByGame, SystemService_SetHideMouseCursorByGame),
	    HLL_TODO_EXPORT(GetHideMouseCursorByGame, SystemService_GetHideMouseCursorByGame),
	    HLL_EXPORT(SetUsePower2Texture, SystemService_SetUsePower2Texture),
	    HLL_TODO_EXPORT(GetUsePower2Texture, SystemService_GetUsePower2Texture),
	    HLL_EXPORT(SetAntiAliasingMode, SystemService_SetAntiAliasingMode),
	    HLL_TODO_EXPORT(GetAntiAliasingMode, SystemService_GetAntiAliasingMode),
	    HLL_EXPORT(SetWindowSetting, SystemService_SetWindowSetting),
	    HLL_EXPORT(GetWindowSetting, SystemService_GetWindowSetting),
	    HLL_EXPORT(SetMouseCursorConfig, SystemService_SetMouseCursorConfig),
	    HLL_EXPORT(GetMouseCursorConfig, SystemService_GetMouseCursorConfig),
	    HLL_TODO_EXPORT(RunProgram, SystemService_RunProgram),
	    HLL_TODO_EXPORT(IsOpenedMutex, SystemService_IsOpenedMutex),
	    HLL_EXPORT(GetGameFolderPath, SystemService_GetGameFolderPath),
	    HLL_EXPORT(GetDate, get_date),
	    HLL_EXPORT(GetTime, SystemService_GetTime),
	    HLL_EXPORT(IsResetOnce, SystemService_IsResetOnce),
	    HLL_EXPORT(OpenPlayingManual, SystemService_OpenPlayingManual),
	    HLL_EXPORT(IsExistPlayingManual, SystemService_IsExistPlayingManual),
	    HLL_EXPORT(IsExistSystemMessage, SystemService_IsExistSystemMessage),
	    HLL_TODO_EXPORT(PopSystemMessage, SystemService_PopSystemMessage),
	    HLL_EXPORT(RestrainScreensaver, SystemService_RestrainScreensaver),
	    HLL_EXPORT(AddBackupSaveFileName, SystemService_AddBackupSaveFileName),
	    HLL_EXPORT(BackupSaveFile, SystemService_BackupSaveFile),
	    HLL_TODO_EXPORT(Debug_GetUseVideoMemorySize, SystemService_Debug_GetUseVideoMemorySize),
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
}
