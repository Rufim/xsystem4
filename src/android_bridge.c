/* Мост движок <-> Android: пересылка ADV-текста для TTS,
 * заглушение голосовых микшеров, доступ к переменным VM для читов.
 *
 * Общая часть платформонезависима; эмиттер текста имеет две реализации:
 * JNI (Android) и stdout-заглушка (прочие платформы, для отладки).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "system4.h"
#include "system4/string.h"
#include "system4/utfsjis.h"
#include "android_bridge.h"
#include "mixer.h"

static bool tts_enabled = false;
static bool line_has_voice = false;
static struct string *line_buf = NULL;

static void bridge_emit(const char *utf8_text, bool has_voice);

void bridge_set_tts_enabled(bool on)
{
	tts_enabled = on;
}

static bool contains_nocase(const char *haystack, const char *needle)
{
	size_t nlen = strlen(needle);
	for (const char *p = haystack; *p; p++) {
		if (!strncasecmp(p, needle, nlen))
			return true;
	}
	return false;
}

static bool is_voice_mixer(const char *name)
{
	// имя в SJIS; "voice" — ASCII; 音声/女性/男性 — голосовые категории Daiteikoku
	return contains_nocase(name, "voice") ||
	       strstr(name, "\x89\xb9\x90\xba" /*音声*/) ||
	       strstr(name, "\x8f\x97\x90\xab" /*女性*/) ||
	       strstr(name, "\x92\x6a\x90\xab" /*男性*/);
}

static bool is_music_mixer(const char *name)
{
	return contains_nocase(name, "bgm") ||
	       strstr(name, "\x82\x61\x82\x66\x82\x6c" /*ＢＧＭ*/);
}

void bridge_set_voice_muted(bool on)
{
	int n = mixer_get_numof();
	for (int i = 0; i < n; i++) {
		const char *name = mixer_get_name(i);
		if (!name)
			continue;
		if (getenv("XS4_BRIDGE_DEBUG"))
			printf("[MIXER %d] %s\n", i, name);
		if (is_voice_mixer(name))
			mixer_set_mute(i, on ? 1 : 0);
	}
}

// Приглушение музыки на время фразы TTS. percent — целевой уровень (0..100)
// относительно исходной громкости; on=false восстанавливает исходную.
#define MAX_DUCKED 8
static struct { int idx; int saved_volume; } ducked[MAX_DUCKED];
static int nr_ducked = 0;

void bridge_duck_music(bool on, int percent)
{
	if (on) {
		if (nr_ducked)   // уже приглушено
			return;
		if (percent < 0) percent = 0;
		if (percent > 100) percent = 100;
		int n = mixer_get_numof();
		for (int i = 0; i < n && nr_ducked < MAX_DUCKED; i++) {
			const char *name = mixer_get_name(i);
			if (!name || !is_music_mixer(name))
				continue;
			int vol = 100;
			mixer_get_volume(i, &vol);
			ducked[nr_ducked].idx = i;
			ducked[nr_ducked].saved_volume = vol;
			nr_ducked++;
			mixer_set_volume(i, vol * percent / 100);
		}
	} else {
		for (int i = 0; i < nr_ducked; i++)
			mixer_set_volume(ducked[i].idx, ducked[i].saved_volume);
		nr_ducked = 0;
	}
}

void bridge_adv_add_text(struct string *sjis_text)
{
	if (!sjis_text || !sjis_text->size)
		return;
	if (!line_buf)
		line_buf = string_dup(sjis_text);
	else
		string_append(&line_buf, sjis_text);
}

void bridge_adv_add_voice(int voice_no)
{
	(void)voice_no;
	line_has_voice = true;
}

void bridge_adv_line_break(void)
{
#ifndef __ANDROID__
	// XS4_MUTE_TEST=1: разово заглушить голос и приглушить музыку до 15%
	// на первой же строке (отладка)
	static bool mute_tested = false;
	if (!mute_tested && getenv("XS4_MUTE_TEST")) {
		mute_tested = true;
		bridge_set_voice_muted(true);
		bridge_duck_music(true, 15);
	}
#endif
	if (line_buf && line_buf->size) {
		char *utf8 = sjis2utf(line_buf->text, line_buf->size);
		bridge_emit(utf8, line_has_voice);
		free(utf8);
	}
	if (line_buf) {
		free_string(line_buf);
		line_buf = NULL;
	}
	line_has_voice = false;
}

#ifndef __ANDROID__
static void bridge_emit(const char *utf8_text, bool has_voice)
{
	if (!getenv("XS4_BRIDGE_DEBUG"))
		return;
	printf("[ADV] voice=%d |%s|\n", has_voice, utf8_text);
	fflush(stdout);
}
#else /* __ANDROID__ */

#include <jni.h>

static JavaVM *jvm = NULL;
static jclass bridge_class = NULL;      // GlobalRef на NativeBridge
static jmethodID mid_on_adv_text = NULL;

JNIEXPORT void JNICALL
Java_io_github_kichikuou_xsystem4_NativeBridge_nativeInit(JNIEnv *env, jclass cls)
{
	(*env)->GetJavaVM(env, &jvm);
	bridge_class = (*env)->NewGlobalRef(env, cls);
	mid_on_adv_text = (*env)->GetStaticMethodID(env, cls, "onAdvText",
	                                            "(Ljava/lang/String;Z)V");
}

JNIEXPORT void JNICALL
Java_io_github_kichikuou_xsystem4_NativeBridge_nativeSetTts(
		JNIEnv *env, jclass cls, jboolean on)
{
	(void)env; (void)cls;
	bridge_set_tts_enabled(on);
	bridge_set_voice_muted(on);
}

JNIEXPORT void JNICALL
Java_io_github_kichikuou_xsystem4_NativeBridge_nativeDuckMusic(
		JNIEnv *env, jclass cls, jboolean on, jint percent)
{
	(void)env; (void)cls;
	bridge_duck_music(on, percent);
}

static JNIEnv *bridge_env(void)
{
	if (!jvm)
		return NULL;
	JNIEnv *env;
	if ((*jvm)->GetEnv(jvm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
		// поток VM живёт до конца процесса — Detach не требуется
		if ((*jvm)->AttachCurrentThread(jvm, &env, NULL) != JNI_OK)
			return NULL;
	}
	return env;
}

static void bridge_emit(const char *utf8_text, bool has_voice)
{
	if (!tts_enabled || !mid_on_adv_text)
		return;
	JNIEnv *env = bridge_env();
	if (!env)
		return;
	jstring jtext = (*env)->NewStringUTF(env, utf8_text);
	if (!jtext) {
		(*env)->ExceptionClear(env);
		return;
	}
	(*env)->CallStaticVoidMethod(env, bridge_class, mid_on_adv_text,
	                             jtext, (jboolean)has_voice);
	if ((*env)->ExceptionCheck(env))
		(*env)->ExceptionClear(env);
	(*env)->DeleteLocalRef(env, jtext);
}
#endif /* __ANDROID__ */
