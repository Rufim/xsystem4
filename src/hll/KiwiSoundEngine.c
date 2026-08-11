/* Copyright (C) 2021 Nunuhara Cabbage <nunuhara@haniwa.technology>
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

#include <stdio.h>
#include <string.h>

#include "system4.h"
#include "system4/ain.h"
#include "system4/archive.h"
#include "system4/ex.h"
#include "system4/hashtable.h"
#include "system4/string.h"

#include "vm.h"
#include "hll.h"
#include "asset_manager.h"
#include "audio.h"
#include "mixer.h"
#include "xsystem4.h"

HLL_WARN_UNIMPLEMENTED( , void, KiwiSoundEngine, SetGlobalFocus, possibly_unused int nNum);

//int KiwiSoundEngine_Sound_GetGroupNum(int nCh);
//static bool KiwiSoundEngine_Sound_PrepareFromFile(int ch, struct string *filename);

static int KiwiSoundEngine_GetMasterGroup(void)
{
	return 0;
}

static int KiwiSoundEngine_GetBGMGroup(void)
{
	return 0;
}

static int KiwiSoundEngine_GetSEGroup(void)
{
	return 0;
}

static int KiwiSoundEngine_GetVoiceGroup(void)
{
	return 0;
}

static int KiwiSoundEngine_GetGimicSEGroup(void)
{
	return 0;
}

static int KiwiSoundEngine_GetBackVoiceGroup(void)
{
	return 0;
}

static bool rance9_bgm_exists(int num)
{
	char name[16];
	snprintf(name, sizeof(name), "%05d", num);
	return asset_exists_by_name(ASSET_BGM, name, NULL);
}

static int rance9_bgm_prepare(int ch, int num)
{
	char name[16];
	snprintf(name, sizeof(name), "%05d", num);
	struct archive_data *dfile = asset_get_by_name(ASSET_BGM, name, NULL);
	if (!dfile)
		return 0;
	return bgm_prepare_from_archive_data(ch, dfile);
}

static bool rance9_wav_exists(int num)
{
	char name[16];
	snprintf(name, sizeof(name), "%05d", num);
	return asset_exists_by_name(ASSET_SOUND, name, NULL);
}

static int rance9_wav_prepare(int ch, int num)
{
	char name[16];
	snprintf(name, sizeof(name), "%05d", num);
	struct archive_data *dfile = asset_get_by_name(ASSET_SOUND, name, NULL);
	if (!dfile)
		return 0;
	return wav_prepare_from_archive_data(ch, dfile);
}

/*
 * Newer games (e.g. Tsumamigui 3) use a flat sound API keyed by an integer
 * ID instead of the namespaced Music_ and Sound_ API. All audio (BGM, SE and
 * opening songs) lives in the ASSET_SOUND archive, addressed by basename (no
 * extension). The flat ID doubles as a channel id in the shared wav pool
 * (audio.c's id_pool grows on demand), so playback maps straight onto the
 * existing wav_* backend. GetFreeID/GetFreeSeID hand out a currently-idle id
 * from the [Min,Max] range the game requests, which keeps the id space bounded
 * and lets finished one-shots be reused.
 */
#define KSE_SND_TRACE() (getenv("XSYS4_SND_TRACE") != NULL)

static int kse_get_free_id(int min_id, int max_id)
{
	if (max_id < min_id)
		max_id = min_id;
	for (int id = min_id; id <= max_id; id++) {
		if (!wav_is_playing(id))
			return id;
	}
	return min_id;
}

static int KiwiSoundEngine_GetFreeID(int min_id, int max_id)
{
	int id = kse_get_free_id(min_id, max_id);
	if (KSE_SND_TRACE()) NOTICE("KSE GetFreeID(%d,%d) -> %d", min_id, max_id, id);
	return id;
}

static int KiwiSoundEngine_GetFreeSeID(int min_id, int max_id)
{
	return KiwiSoundEngine_GetFreeID(min_id, max_id);
}

// Load a sound by name from the Sound archive and prepare it on channel `id`.
/*
 * Звук по ИМЕНИ ищется в ОБОИХ архивах — сначала эффекты/музыка, затем озвучка.
 * У Tsumamigui 3 это два разных файла: `Tsumamigui3Sound.afa` (музыка и SE) и
 * `Tsumamigui3Voice.afa` (18 848 реплик вида `10001.ogg`). Поиск только по
 * ASSET_SOUND озвучку не находил вовсе: игра писала себе в лог
 * `警告 VOICE[10001]ロード失敗 ＠ message::detail::VOICE`, и сцены шли молча при
 * работающей музыке — снаружи это выглядит как «озвучки в игре нет».
 * Порядок важен: имена SE и реплик не пересекаются, но SE запрашиваются чаще.
 */
/*
 * ТАБЛИЦА «ИМЯ ФАЙЛА → ЗВУКОВАЯ ГРУППА» (FINDINGS §5ag).
 *
 * Игра спрашивает группу файла через `GetGroupNumFromFile`, и у оригинала ответ
 * лежит ДАННЫМИ: внутри самих звуковых архивов есть по файлу `.ex` —
 * `timemap_sound.ex` в архиве музыки/эффектов и `timemap_voice.ex` в архиве
 * озвучки, оба с одной таблицей:
 *
 *   table SoundInfo = {
 *       { indexed string Name, int Group, int LoopBeginSample = 0, int LoopEndSample = 0 },
 *       { "03_日常_05_TD_4416_ST_BPM114", 1, 0, 4456422 },
 *       { "10001",                        6, 0,  262446 },
 *
 * Никакой эвристики по префиксам имён не нужно. У Tsumamigui 3 сходится точно:
 * 25 записей с группой 1 (музыка) и 119 с группой 2 (эффекты) в `timemap_sound`,
 * а в `timemap_voice` — 6/7/8/9 для четырёх персонажей и 10…13 для их «фоновых»
 * близнецов. Нумерация подтверждена кодом игры (`config::GetBGMVolume` → группа 1,
 * `GetSEVolume` → 2, `GetVoiceVolume` → 3, `GetBackVoiceVolume` → 5).
 *
 * Оттуда же берутся точки петли: у игр, грузящих звук ПО ИМЕНИ, файла `.bgi` нет
 * вовсе, поэтому иначе музыка зацикливалась бы не по авторским точкам.
 */
struct kse_sound_info {
	int group;
	int loop_begin;
	int loop_end;
};

static struct hash_table *kse_sound_info;   // имя → struct kse_sound_info*
static bool kse_sound_info_loaded = false;

static void kse_load_timemap(enum asset_type type, const char *name)
{
	struct archive_data *dfile = asset_get_by_name(type, name, NULL);
	if (!dfile) {
		if (KSE_SND_TRACE()) NOTICE("KSE: %s отсутствует в архиве", name);
		return;
	}
	struct ex *ex = ex_read(dfile->data, dfile->size);
	archive_free_data(dfile);
	if (!ex) {
		WARNING("KiwiSoundEngine: не разобрать %s", name);
		return;
	}
	struct ex_value *v = ex_get(ex, "SoundInfo");
	if (!v || v->type != EX_TABLE) {
		WARNING("KiwiSoundEngine: в %s нет таблицы SoundInfo", name);
		ex_free(ex);
		return;
	}
	struct ex_table *t = v->t;
	int nr = 0;
	// Столбцы по порядку схемы: Name, Group, LoopBeginSample, LoopEndSample.
	// `rows` — массив указателей на массивы значений, ширина одна для всех строк.
	if (t->nr_columns < 2) {
		WARNING("KiwiSoundEngine: у SoundInfo в %s мало столбцов (%u)", name, t->nr_columns);
		ex_free(ex);
		return;
	}
	for (unsigned row = 0; row < t->nr_rows; row++) {
		struct ex_value *cols = t->rows[row];
		if (!cols)
			continue;
		struct ex_value *nm = &cols[0];
		if (nm->type != EX_STRING || !nm->s)
			continue;
		struct kse_sound_info *si = xcalloc(1, sizeof(*si));
		si->group      = cols[1].type == EX_INT ? cols[1].i : 0;
		si->loop_begin = t->nr_columns > 2 && cols[2].type == EX_INT ? cols[2].i : 0;
		si->loop_end   = t->nr_columns > 3 && cols[3].type == EX_INT ? cols[3].i : 0;
		struct ht_slot *slot = ht_put(kse_sound_info, nm->s->text, NULL);
		if (slot->value)
			free(slot->value);
		slot->value = si;
		nr++;
	}
	if (KSE_SND_TRACE()) NOTICE("KSE: %s — записей %d", name, nr);
	ex_free(ex);
}

static const struct kse_sound_info *kse_get_sound_info(const char *name)
{
	if (!kse_sound_info_loaded) {
		kse_sound_info_loaded = true;
		kse_sound_info = ht_create(4096);
		kse_load_timemap(ASSET_SOUND, "timemap_sound");
		kse_load_timemap(ASSET_VOICE, "timemap_voice");
	}
	if (!name)
		return NULL;
	struct ht_slot *slot = ht_put(kse_sound_info, name, NULL);
	return slot->value;
}

static struct archive_data *kse_get_sound_by_name(const char *name)
{
	struct archive_data *dfile = asset_get_by_name(ASSET_SOUND, name, NULL);
	if (!dfile)
		dfile = asset_get_by_name(ASSET_VOICE, name, NULL);
	return dfile;
}

static bool kse_prepare_name(int id, struct string *name)
{
	if (!name || id < 0)
		return false;
	struct archive_data *dfile = kse_get_sound_by_name(name->text);
	if (!dfile) {
		if (KSE_SND_TRACE()) NOTICE("KSE: sound '%s' not found", name->text);
		return false;
	}
	// wav_prepare_from_archive_data takes ownership of dfile.
	if (!wav_prepare_from_archive_data(id, dfile))
		return false;
	// Канал микшера = звуковая группа файла, и точки петли — из той же таблицы.
	// Без этого весь звук играл на канале 0 и ползунки не могли развести музыку,
	// эффекты и озвучку (FINDINGS §5ag).
	const struct kse_sound_info *si = kse_get_sound_info(name->text);
	if (si) {
		wav_set_mixer(id, si->group);
		if (si->loop_end > 0) {
			wav_set_loop_start_pos(id, si->loop_begin);
			wav_set_loop_end_pos(id, si->loop_end);
		}
		if (KSE_SND_TRACE()) {
			// Громкость и мьют группы — прямо в трассу: «запустилось, но не слышно»
			// иначе не отличить от «глушится микшером», а по логу это видно сразу.
			int vol = -1, mute = -1, mvol = -1, mmute = -1;
			mixer_get_volume(si->group, &vol);
			mixer_get_mute(si->group, &mute);
			mixer_get_volume(0, &mvol);
			mixer_get_mute(0, &mmute);
			NOTICE("KSE route id=%d '%s' -> группа %d (петля %d..%d) "
			       "громкость группы %d%% mute=%d, группа 0: %d%% mute=%d",
			       id, name->text, si->group, si->loop_begin, si->loop_end,
			       vol, mute, mvol, mmute);
		}
	}
	return true;
}

static bool KiwiSoundEngine_flat_IsExistFile(struct string *s)
{
	// Оба архива — см. комментарий у kse_get_sound_by_name. Игра спрашивает этим
	// «есть ли реплика», и ответ «нет» гасил озвучку ещё до попытки загрузки.
	return s && (asset_exists_by_name(ASSET_SOUND, s->text, NULL)
			|| asset_exists_by_name(ASSET_VOICE, s->text, NULL));
}
static bool KiwiSoundEngine_flat_IsExistID(int id) { (void)id; return true; }
static bool KiwiSoundEngine_flat_IsExistSeID(int id) { (void)id; return true; }
static bool KiwiSoundEngine_flat_IsPlay(int id) { return wav_is_playing(id); }
static bool KiwiSoundEngine_flat_IsPlaySe(int id) { return wav_is_playing(id); }
static bool KiwiSoundEngine_flat_IsFade(int id) { return wav_is_fading(id); }
static bool KiwiSoundEngine_flat_IsPause(int id) { return wav_is_paused(id); }
static bool KiwiSoundEngine_flat_Prepare(int id, struct string *s, bool streaming)
{
	(void)streaming;  // we always decode from the archive; no separate stream path
	if (KSE_SND_TRACE()) NOTICE("KSE Prepare(id=%d, name='%s', streaming=%d)", id, s?s->text:"(null)", streaming);
	return kse_prepare_name(id, s);
}
static bool KiwiSoundEngine_flat_Unprepare(int id) { wav_unprepare(id); return true; }
static bool KiwiSoundEngine_flat_Play(int id) { bool r = wav_play(id); if (KSE_SND_TRACE()) NOTICE("KSE Play(id=%d) -> %d, len=%dms, playing=%d", id, r, wav_get_time_length(id), wav_is_playing(id)); return r; }
static bool KiwiSoundEngine_flat_PlaySe(int id, struct string *s)
{
	if (KSE_SND_TRACE()) NOTICE("KSE PlaySe(id=%d, name='%s')", id, s?s->text:"(null)");
	if (!kse_prepare_name(id, s))
		return false;
	wav_set_loop_count(id, 1);  // one-shot
	return wav_play(id);
}
static bool KiwiSoundEngine_flat_Stop(int id) { wav_stop(id); return true; }
static bool KiwiSoundEngine_flat_StopSe(int id) { wav_stop(id); return true; }
static bool KiwiSoundEngine_flat_Pause(int id) { wav_pause(id); return true; }
static bool KiwiSoundEngine_flat_Restart(int id) { wav_restart(id); return true; }
static bool KiwiSoundEngine_flat_Fade(int id, int time, float volume, bool stop, int fade_type)
{
	(void)fade_type;
	int vol = (int)(volume <= 1.0f ? volume * 100.0f + 0.5f : volume);
	wav_fade(id, time, vol, stop);
	return true;
}
static bool KiwiSoundEngine_flat_StopFade(int id) { wav_stop_fade(id); return true; }
static bool KiwiSoundEngine_flat_Seek(int id, int millisec) { wav_seek(id, millisec); return true; }
static bool KiwiSoundEngine_flat_SetLoopCount(int id, int n)
{
	// Счётчик повторов — то, чем экран MUSIC реализует リピート１曲: 0 = бесконечно.
	if (KSE_SND_TRACE()) NOTICE("KSE SetLoopCount(id=%d, n=%d)", id, n);
	wav_set_loop_count(id, n);
	return true;
}
static bool KiwiSoundEngine_flat_SetSeParam(int a, int b, bool c) { (void)a; (void)b; (void)c; return true; }
static int KiwiSoundEngine_flat_GetLength(int id) { return wav_get_time_length(id); }
/*
 * Длина трека ПО ИМЕНИ ФАЙЛА, без занятия канала. Была заглушка `return 0`, и экран
 * MUSIC печатал знаменатель `タイム 00：27／00：00`: игра берёт общее время через
 * `AFL_Sound_GetLength(dataName)` → сюда, а канальный `GetLength(id)` (он же
 * `sound::detail::CASBGM@GetLength`) отдаёт верные 192000 мс и остаётся нетронутым.
 *
 * Канал под это не заводим: `channel_open_archive_data` читает лишь заголовок через
 * libsndfile, а длина считается из `info.frames`. Ответ кэшируем — экран спрашивает
 * длину каждый кадр, а распаковка записи из .afa на кадр стоила бы дороже всего
 * остального вместе взятого.
 */
static struct hash_table *kse_length_cache;

static int KiwiSoundEngine_flat_GetLengthFromFile(struct string *s)
{
	if (!s || !s->text[0])
		return 0;
	if (!kse_length_cache)
		kse_length_cache = ht_create(256);
	struct ht_slot *slot = ht_put(kse_length_cache, s->text, NULL);
	if (slot->value)
		return (int)(intptr_t)slot->value - 1;   // 0 мс тоже кэшируем, отсюда +1

	int len = 0;
	struct archive_data *dfile = kse_get_sound_by_name(s->text);
	if (dfile) {
		// channel_open_archive_data забирает dfile себе (и освобождает при ошибке).
		struct channel *ch = channel_open_archive_data(dfile);
		if (ch) {
			len = channel_get_time_length(ch);
			channel_close(ch);
		}
	} else if (KSE_SND_TRACE()) {
		NOTICE("KSE GetLengthFromFile: '%s' не найден", s->text);
	}
	if (KSE_SND_TRACE())
		NOTICE("KSE GetLengthFromFile('%s') -> %d мс", s->text, len);
	slot->value = (void *)(intptr_t)(len + 1);
	return len;
}
static int KiwiSoundEngine_flat_GetPos(int id) { return wav_get_pos(id); }
static int KiwiSoundEngine_flat_GetLoopCount(int id) { return wav_get_loop_count(id); }
static int KiwiSoundEngine_flat_GetGroupNum(int id) { (void)id; return 0; }
static int KiwiSoundEngine_flat_GetGroupNumFromFile(struct string *s)
{
	// Была заглушка `return 0`, из-за которой игра считала все звуки группой 0
	// (мастер) — в частности `IsMuteVoice` проверяла мьют не той группы.
	const struct kse_sound_info *si = kse_get_sound_info(s ? s->text : NULL);
	return si ? si->group : 0;
}
static float KiwiSoundEngine_flat_GetGroupVolume(int id) { (void)id; return 1.0f; }
static int KiwiSoundEngine_flat_MillisecondsToSamples(int a, int b) { (void)a; (void)b; return 0; }
static int KiwiSoundEngine_flat_GetSoundFileName(int id, struct string **out) { (void)id; if (out) { if (*out) free_string(*out); *out = string_ref(&EMPTY_STRING); } return 0; }

// Микшеры (громкость/mute по каналам). У v6/v7 этот API живёт в SystemService, у
// Ixseal он переехал в KiwiSoundEngine — движковый микшер (src/audio_mixer.c) тот же,
// меняется только форма out-параметров: v7 объявляет `ref string/int/bool` (типы
// 20/18/51), Dohna — `wrap<T>` (тип 82), а wrap<скаляр> ffi отдаёт таким же обычным
// указателем (та же форма, что у уже рабочей Parts_GetPartsSize(no,82,82,int)).
// Реализуем по-настоящему, а не заглушкой: у каждого сеттера ЕСТЬ геттер, значит
// no-op отличим — конфиг читает обратно то, что записал.
static int KiwiSoundEngine_GetMixerName(int n, struct string **name)
{
	const char *r = mixer_get_name(n);
	if (!r)
		return 0;
	if (*name)
		free_string(*name);
	*name = make_string(r, strlen(r));
	return 1;
}

static int KiwiSoundEngine_SetMixerName(int n, struct string *name)
{
	return mixer_set_name(n, name ? name->text : "");
}

static bool KiwiSoundEngine_GetMixerDefaultVolume(int n, int *volume)
{
	// Граница — по фактическому числу микшеров (mixer_get_default_volume).
	// Прежде считалась по длине ini-списка, а без ключа `VolumeValancer` она
	// НОЛЬ, поэтому геттер проваливался всегда и игра видела громкость 0,
	// что она трактует как мьют (FINDINGS §5ag).
	return mixer_get_default_volume(n, volume);
}

static void KiwiSoundEngine_PreLink(void);

HLL_LIBRARY(KiwiSoundEngine,
	    HLL_EXPORT(_PreLink, KiwiSoundEngine_PreLink),
	    HLL_EXPORT(SetGlobalFocus, KiwiSoundEngine_SetGlobalFocus),
	    HLL_EXPORT(GetMixerNumof, mixer_get_numof),
	    HLL_EXPORT(GetMixerName, KiwiSoundEngine_GetMixerName),
	    HLL_EXPORT(GetMixerVolume, mixer_get_volume),
	    HLL_EXPORT(GetMixerDefaultVolume, KiwiSoundEngine_GetMixerDefaultVolume),
	    HLL_EXPORT(GetMixerMute, mixer_get_mute),
	    HLL_EXPORT(SetMixerName, KiwiSoundEngine_SetMixerName),
	    HLL_EXPORT(SetMixerVolume, mixer_set_volume),
	    HLL_EXPORT(SetMixerMute, mixer_set_mute),
	    HLL_EXPORT(Music_IsExist, bgm_exists),
	    HLL_EXPORT(Music_Prepare, bgm_prepare),
	    HLL_EXPORT(Music_Unprepare, bgm_unprepare),
	    HLL_EXPORT(Music_Play, bgm_play),
	    HLL_EXPORT(Music_Stop, bgm_stop),
	    HLL_EXPORT(Music_IsPlay, bgm_is_playing),
	    HLL_EXPORT(Music_SetLoopCount, bgm_set_loop_count),
	    HLL_EXPORT(Music_GetLoopCount, bgm_get_loop_count),
	    HLL_EXPORT(Music_SetLoopStartPos, bgm_set_loop_start_pos),
	    HLL_EXPORT(Music_SetLoopEndPos, bgm_set_loop_end_pos),
	    HLL_EXPORT(Music_Fade, bgm_fade),
	    HLL_EXPORT(Music_StopFade, bgm_stop_fade),
	    HLL_EXPORT(Music_IsFade, bgm_is_fading),
	    HLL_EXPORT(Music_Pause, bgm_pause),
	    HLL_EXPORT(Music_Restart, bgm_restart),
	    HLL_EXPORT(Music_IsPause, bgm_is_paused),
	    HLL_EXPORT(Music_GetPos, bgm_get_pos),
	    HLL_EXPORT(Music_GetLength, bgm_get_length),
	    HLL_EXPORT(Music_GetSamplePos, bgm_get_sample_pos),
	    HLL_EXPORT(Music_GetSampleLength, bgm_get_sample_length),
	    HLL_EXPORT(Music_Seek, bgm_seek),
	    //HLL_EXPORT(Music_MillisecondsToSamples, KiwiSoundEngine_Music_MillisecondsToSamples),
	    //HLL_EXPORT(Music_GetFormat, KiwiSoundEngine_Music_GetFormat),
	    HLL_EXPORT(Sound_IsExist, wav_exists),
	    HLL_EXPORT(Sound_Prepare, wav_prepare),
	    HLL_EXPORT(Sound_Unprepare, wav_unprepare),
	    HLL_EXPORT(Sound_Play, wav_play),
	    HLL_EXPORT(Sound_Stop, wav_stop),
	    HLL_EXPORT(Sound_IsPlay, wav_is_playing),
	    HLL_EXPORT(Sound_Fade, wav_fade),
	    HLL_EXPORT(Sound_StopFade, wav_stop_fade),
	    HLL_EXPORT(Sound_IsFade, wav_is_fading),
	    HLL_EXPORT(Sound_GetTimeLength, wav_get_time_length),
	    HLL_TODO_EXPORT(Sound_GetGroupNum, KiwiSoundEngine_Sound_GetGroupNum),
	    HLL_EXPORT(Sound_GetGroupNumFromDataNum, wav_get_group_num_from_data_num),
	    HLL_TODO_EXPORT(Sound_PrepareFromFile, KiwiSoundEngine_Sound_PrepareFromFile),
	    HLL_EXPORT(Sound_GetDataLength, wav_get_data_length),
	    // Единственная СТРОГО достижимая дыра Dohna Dohna (FINDINGS §5y).
	    // Тот же микшер, что у `SystemService.GetMixerMute` — Ixseal просто
	    // объявляет его ещё и здесь, реализация общая.
	    HLL_EXPORT(GetMixerMute, mixer_get_mute),
	    HLL_EXPORT(SetMixerMute, mixer_set_mute),
	    HLL_EXPORT(GetMasterGroup, KiwiSoundEngine_GetMasterGroup),
	    HLL_EXPORT(GetBGMGroup, KiwiSoundEngine_GetBGMGroup),
	    HLL_EXPORT(GetSEGroup, KiwiSoundEngine_GetSEGroup),
	    HLL_EXPORT(GetVoiceGroup, KiwiSoundEngine_GetVoiceGroup),
	    HLL_EXPORT(GetGimicSEGroup, KiwiSoundEngine_GetGimicSEGroup),
	    HLL_EXPORT(GetBackVoiceGroup, KiwiSoundEngine_GetBackVoiceGroup),
	    HLL_EXPORT(GetFreeID, KiwiSoundEngine_GetFreeID),
	    HLL_EXPORT(GetFreeSeID, KiwiSoundEngine_GetFreeSeID),
	    HLL_EXPORT(IsExistFile, KiwiSoundEngine_flat_IsExistFile),
	    HLL_EXPORT(IsExistID, KiwiSoundEngine_flat_IsExistID),
	    HLL_EXPORT(IsExistSeID, KiwiSoundEngine_flat_IsExistSeID),
	    HLL_EXPORT(IsPlay, KiwiSoundEngine_flat_IsPlay),
	    HLL_EXPORT(IsPlaySe, KiwiSoundEngine_flat_IsPlaySe),
	    HLL_EXPORT(IsFade, KiwiSoundEngine_flat_IsFade),
	    HLL_EXPORT(IsPause, KiwiSoundEngine_flat_IsPause),
	    HLL_EXPORT(Prepare, KiwiSoundEngine_flat_Prepare),
	    HLL_EXPORT(Unprepare, KiwiSoundEngine_flat_Unprepare),
	    HLL_EXPORT(Play, KiwiSoundEngine_flat_Play),
	    HLL_EXPORT(PlaySe, KiwiSoundEngine_flat_PlaySe),
	    HLL_EXPORT(Stop, KiwiSoundEngine_flat_Stop),
	    HLL_EXPORT(StopSe, KiwiSoundEngine_flat_StopSe),
	    HLL_EXPORT(Pause, KiwiSoundEngine_flat_Pause),
	    HLL_EXPORT(Restart, KiwiSoundEngine_flat_Restart),
	    HLL_EXPORT(Fade, KiwiSoundEngine_flat_Fade),
	    HLL_EXPORT(StopFade, KiwiSoundEngine_flat_StopFade),
	    HLL_EXPORT(Seek, KiwiSoundEngine_flat_Seek),
	    HLL_EXPORT(SetLoopCount, KiwiSoundEngine_flat_SetLoopCount),
	    HLL_EXPORT(SetSeParam, KiwiSoundEngine_flat_SetSeParam),
	    HLL_EXPORT(GetLength, KiwiSoundEngine_flat_GetLength),
	    HLL_EXPORT(GetLengthFromFile, KiwiSoundEngine_flat_GetLengthFromFile),
	    HLL_EXPORT(GetPos, KiwiSoundEngine_flat_GetPos),
	    HLL_EXPORT(GetLoopCount, KiwiSoundEngine_flat_GetLoopCount),
	    HLL_EXPORT(GetGroupNum, KiwiSoundEngine_flat_GetGroupNum),
	    HLL_EXPORT(GetGroupNumFromFile, KiwiSoundEngine_flat_GetGroupNumFromFile),
	    HLL_EXPORT(GetGroupVolume, KiwiSoundEngine_flat_GetGroupVolume),
	    HLL_EXPORT(MillisecondsToSamples, KiwiSoundEngine_flat_MillisecondsToSamples),
	    HLL_EXPORT(GetSoundFileName, KiwiSoundEngine_flat_GetSoundFileName)
	);

static void KiwiSoundEngine_PreLink(void)
{
	int libno = ain_get_library(ain, "KiwiSoundEngine");
	if (libno < 0)
		return;
	if (ain_get_library_function(ain, libno, "Sound_Seek") >= 0) {
		static_library_replace(&lib_KiwiSoundEngine, "Music_IsExist", rance9_bgm_exists);
		static_library_replace(&lib_KiwiSoundEngine, "Music_Prepare", rance9_bgm_prepare);
		static_library_replace(&lib_KiwiSoundEngine, "Sound_IsExist", rance9_wav_exists);
		static_library_replace(&lib_KiwiSoundEngine, "Sound_Prepare", rance9_wav_prepare);
	}
}
