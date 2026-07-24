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
 * Newer System 4 games (v11+, e.g. Healing Touch, Dohna Dohna) expose the
 * engine "system" services as an ordinary HLL library (called via CALLHLL)
 * rather than the legacy CALLSYS syscalls. The function set mirrors the
 * syscalls implemented in vm.c's system_call(); this library re-exposes them
 * through the standard libffi HLL ABI (typed C signatures, marshalled args).
 * Older games keep using CALLSYS and never link this library, so it is purely
 * additive.
 */

#include <string.h>
#include <stdlib.h>

#include "system4.h"
#include "system4/string.h"
#include "system4/file.h"
#include "system4/utfsjis.h"

#include "xsystem4.h"
#include "vm.h"
#include "vm/page.h"
#include "vm/heap.h"
#include "savedata.h"
#include "input.h"
#include "hll.h"

/* Declared under VM_PRIVATE in vm.h; re-declare for the HLL wrappers. */
int vm_save_image(const char *key, const char *path);
void vm_load_image(const char *key, const char *path);
struct page *vm_load_image_comments(const char *key, const char *path, int *success);
int vm_write_image_comments(const char *key, const char *path, struct page *comments);
extern int SDL_OpenURL(const char *url);

static struct string *sys_ref(struct string *s)
{
	return string_ref(s ? s : &EMPTY_STRING);
}

/* --- diagnostics / flow --- */

static struct string *System_Output(struct string *text)
{
	log_message("stdout", "%s", display_sjis0(text->text));
	return sys_ref(text);
}

static struct string *System_OutputLine(struct string *text)
{
	log_message("stdout", "%s\n", display_sjis0(text->text));
	return sys_ref(text);
}

static struct string *System_MsgBox(struct string *text)
{
	NOTICE("system.MsgBox: %s", display_sjis0(text->text));
	return sys_ref(text);
}

static int System_MsgBoxOkCancel(struct string *text)
{
	NOTICE("system.MsgBoxOkCancel: %s -> OK", display_sjis0(text->text));
	return 1; // auto-confirm
}

static struct string *System_Error(struct string *text)
{
	// system.Error is a script-level assert (has a Continue affordance); log
	// and continue by default so a single failing assert doesn't wedge the game.
	sys_warning("*GAME ERROR*: %s\n", display_sjis0(text->text));
	if (getenv("XSYS4_STOP_ON_GAME_ERROR"))
		vm_exit(1);
	return sys_ref(text);
}

static void System_Exit(int result)
{
	vm_exit(result);
}

static void System_Reset(void)
{
	vm_reset();
}

static void System_Peek(void)
{
	handle_events();
}

static void System_PeekAll(void)
{
	handle_events();
}

static void System_Sleep(int ms)
{
	vm_sleep(ms);
}

static void System_OpenWeb(struct string *url)
{
	SDL_OpenURL(url->text);
}

static bool System_IsDebugMode(void)
{
	return false;
}

static int System_GetTime(void)
{
	return vm_time();
}

static struct string *System_GetGameName(void)
{
	return make_string(config.game_name, strlen(config.game_name));
}

static struct string *System_GetSaveFolderName(void)
{
	if (!config.save_dir)
		return string_ref(&EMPTY_STRING);
	char *sjis = utf2sjis(config.save_dir, strlen(config.save_dir));
	struct string *s = make_string(sjis, strlen(sjis));
	free(sjis);
	return s;
}

static struct string *System_GetFuncStackName(int index)
{
	(void)index;
	return string_ref(&EMPTY_STRING);
}

static bool System_ExistFunc(struct string *name)
{
	return ain_get_function(ain, name->text) > 0;
}

/* --- save files --- */

static bool System_ExistSaveFile(struct string *filename)
{
	char *path = savedir_path(filename->text);
	bool r = file_exists(path);
	free(path);
	return r;
}

static bool System_DeleteSaveFile(struct string *filename)
{
	return delete_save_file(filename->text);
}

static bool System_CopySaveFile(struct string *dst, struct string *src)
{
	char *u_src = savedir_path(src->text);
	char *u_dst = savedir_path(dst->text);
	bool r = file_copy(u_src, u_dst);
	free(u_src);
	free(u_dst);
	return r;
}

static bool System_BackupSaveFile(struct string *dst, struct string *src)
{
	return System_CopySaveFile(dst, src);
}

static bool System_ResumeSave(struct string *key, struct string *filename, struct page **comment)
{
	(void)comment;
	return vm_save_image(key->text, filename->text);
}

static void System_ResumeLoad(struct string *key, struct string *filename)
{
	vm_load_image(key->text, filename->text);
}

static bool System_ResumeReadComment(struct string *key, struct string *filename, struct page **comment)
{
	int success = 0;
	struct page *p = vm_load_image_comments(key->text, filename->text, &success);
	if (*comment) {
		delete_page_vars(*comment);
		free_page(*comment);
	}
	*comment = p;
	return success;
}

static bool System_ResumeWriteComment(struct string *key, struct string *filename, struct page **comment)
{
	return vm_write_image_comments(key->text, filename->text, comment ? *comment : NULL);
}

static bool System_GroupSave(struct string *key, struct string *filename, struct string *group, int *n)
{
	return save_globals(key->text, filename->text, group->text, n);
}

static bool System_GroupLoad(struct string *key, struct string *filename, struct string *group, int *n)
{
	return load_globals(key->text, filename->text, group->text, n);
}

/* Save-comment / struct-serialization variants: not yet backed by the save
 * subsystem. Stubbed to a benign failure so the game falls back gracefully. */
static bool System_WriteGroupSaveComment(struct string *key, struct string *filename, struct string *group)
{
	(void)key; (void)filename; (void)group;
	return false;
}

static bool System_ReadGroupSaveComment(struct string *key, struct string *filename, struct page **comment)
{
	(void)key; (void)filename; (void)comment;
	return false;
}

static bool System_SerializeStruct(struct string *filename, int struct_type, bool b)
{
	(void)filename; (void)struct_type; (void)b;
	return false;
}

static bool System_DeserializeStruct(struct string *filename, int struct_type, bool b)
{
	(void)filename; (void)struct_type; (void)b;
	return false;
}

static bool System_WriteSerializeStructComment(struct string *filename, struct string *comment, bool b)
{
	(void)filename; (void)comment; (void)b;
	return false;
}

static bool System_ReadSerializeStructComment(struct string *filename, struct page **comment, bool b)
{
	(void)filename; (void)comment; (void)b;
	return false;
}

HLL_LIBRARY(system,
	    HLL_EXPORT(ResumeSave, System_ResumeSave),
	    HLL_EXPORT(ResumeLoad, System_ResumeLoad),
	    HLL_EXPORT(Peek, System_Peek),
	    HLL_EXPORT(PeekAll, System_PeekAll),
	    HLL_EXPORT(Exit, System_Exit),
	    HLL_EXPORT(Reset, System_Reset),
	    HLL_EXPORT(IsDebugMode, System_IsDebugMode),
	    HLL_EXPORT(ResumeWriteComment, System_ResumeWriteComment),
	    HLL_EXPORT(ResumeReadComment, System_ResumeReadComment),
	    HLL_EXPORT(GroupSave, System_GroupSave),
	    HLL_EXPORT(GroupLoad, System_GroupLoad),
	    HLL_EXPORT(WriteGroupSaveComment, System_WriteGroupSaveComment),
	    HLL_EXPORT(ReadGroupSaveComment, System_ReadGroupSaveComment),
	    HLL_EXPORT(SerializeStruct, System_SerializeStruct),
	    HLL_EXPORT(DeserializeStruct, System_DeserializeStruct),
	    HLL_EXPORT(WriteSerializeStructComment, System_WriteSerializeStructComment),
	    HLL_EXPORT(ReadSerializeStructComment, System_ReadSerializeStructComment),
	    HLL_EXPORT(ExistSaveFile, System_ExistSaveFile),
	    HLL_EXPORT(DeleteSaveFile, System_DeleteSaveFile),
	    HLL_EXPORT(CopySaveFile, System_CopySaveFile),
	    HLL_EXPORT(BackupSaveFile, System_BackupSaveFile),
	    HLL_EXPORT(Sleep, System_Sleep),
	    HLL_EXPORT(Output, System_Output),
	    HLL_EXPORT(OutputLine, System_OutputLine),
	    HLL_EXPORT(MsgBox, System_MsgBox),
	    HLL_EXPORT(MsgBoxOkCancel, System_MsgBoxOkCancel),
	    HLL_EXPORT(Error, System_Error),
	    HLL_EXPORT(OpenWeb, System_OpenWeb),
	    HLL_EXPORT(GetSaveFolderName, System_GetSaveFolderName),
	    HLL_EXPORT(GetGameName, System_GetGameName),
	    HLL_EXPORT(GetTime, System_GetTime),
	    HLL_EXPORT(GetFuncStackName, System_GetFuncStackName),
	    HLL_EXPORT(ExistFunc, System_ExistFunc));
