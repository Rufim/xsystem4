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
#include <SDL.h>
#include "system4.h"
#include "system4/ain.h"
#include "system4/string.h"
#include "system4/utfsjis.h"
#include "android_bridge.h"
#include "gfx/font.h"
#include "mixer.h"
#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"

static bool tts_enabled = false;
static bool line_has_voice = false;

static void bridge_trace(const char *fmt, ...);
static void bridge_emit(const char *utf8_text, bool has_voice);

void bridge_set_tts_enabled(bool on)
{
	tts_enabled = on;
}

// Режим «бесконечные события» (Daiteikoku): читается из vm.c (infinite_events_hook).
static bool infinite_events = false;

void bridge_set_infinite_events(bool on)
{
	infinite_events = on;
}

bool bridge_infinite_events_enabled(void)
{
	return infinite_events;
}

// Счётчик посимвольной отрисовки текста (NewFont). Растёт, пока рисуется/
// перерисовывается текст на экране — по нему авто-листание видит активную модалку.
static volatile unsigned newfont_draws = 0;

void bridge_newfont_draw(void)
{
	newfont_draws++;
}

unsigned bridge_ui_draw_count(void)
{
	return newfont_draws;
}

// Счётчик отрисовки именно через NewFont.DrawChar (HLL).
static volatile unsigned nf_char_draws = 0;

void bridge_nf_char_draw(void)
{
	nf_char_draws++;
}

unsigned bridge_nf_char_count(void)
{
	return nf_char_draws;
}

// Буфер фактически отрисованного текста (SJIS, посимвольно из
// _gfx_render_text). Прокачка листания сравнивает его с последней прочитанной
// репликой: второй бокс рисует ХВОСТ той же реплики (текст совпадает — листаем
// дальше), модалка-уведомление рисует чужой текст (замираем).
#define DRAWN_MAX 1024
static char drawn_buf[DRAWN_MAX];
static volatile size_t drawn_len = 0;

void bridge_text_drawn(const char *sjis)
{
	if (!sjis)
		return;
	size_t l = strlen(sjis);
	size_t cur = drawn_len;
	if (cur + l >= DRAWN_MAX)
		return;   // переполнение — хвост не нужен для сравнения
	memcpy(drawn_buf + cur, sjis, l);
	drawn_len = cur + l;
	drawn_buf[drawn_len] = '\0';
}

// Синтетическое нажатие Enter — тем же путём, что и реальный ввод (очередь SDL,
// thread-safe), игра воспринимает его как клик «дальше» в диалоге.
void bridge_advance_message(void)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = SDL_KEYDOWN;
	ev.key.state = SDL_PRESSED;
	ev.key.keysym.sym = SDLK_RETURN;
	ev.key.keysym.scancode = SDL_SCANCODE_RETURN;
	SDL_PushEvent(&ev);
	ev.type = SDL_KEYUP;
	ev.key.state = SDL_RELEASED;
	SDL_PushEvent(&ev);
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
	mixer_suppress_save(true);   // временный мьют голоса — не персистить
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
	mixer_suppress_save(false);
}

// --- Читы: доступ к переменным VM ---

static bool cheat_page_valid(int slot)
{
	return page_index_valid(slot) && heap_get_page(slot) != NULL;
}

static bool is_intlike(enum ain_data_type t)
{
	return t == AIN_INT || t == AIN_BOOL || t == AIN_LONG_INT;
}

int bridge_cheat_read(int page_slot, int varno, int *ok)
{
	*ok = 0;
	if (!cheat_page_valid(page_slot))
		return 0;
	struct page *p = heap_get_page(page_slot);
	if (varno < 0 || varno >= p->nr_vars)
		return 0;
	if (!is_intlike(variable_type(p, varno, NULL, NULL)))
		return 0;
	*ok = 1;
	return p->values[varno].i;
}

bool bridge_cheat_write(int page_slot, int varno, int value)
{
	int ok;
	bridge_cheat_read(page_slot, varno, &ok);
	if (!ok)
		return false;
	heap_get_page(page_slot)->values[varno].i = value;
	return true;
}

int bridge_cheat_list(const char *filter_utf8, struct bridge_var **out, int max)
{
	*out = NULL;
	if (!ain || !cheat_page_valid(0))
		return 0;
	struct page *g = heap_get_page(0);
	struct bridge_var *arr = xcalloc(max, sizeof(struct bridge_var));
	int n = 0;
	for (int i = 0; i < g->nr_vars && i < ain->nr_globals && n < max; i++) {
		if (!is_intlike(variable_type(g, i, NULL, NULL)))
			continue;
		char *name = sjis2utf(ain->globals[i].name, 0);
		if (filter_utf8 && *filter_utf8 && !strstr(name, filter_utf8)) {
			free(name);
			continue;
		}
		arr[n].page_slot = 0;
		arr[n].varno = i;
		arr[n].name_utf8 = name;
		arr[n].value = g->values[i].i;
		n++;
	}
	*out = arr;
	return n;
}

// Кандидаты скана (живут между вызовами new/narrow)
#define SCAN_MAX 100000
static struct { int page_slot; int varno; } *scan_cands = NULL;
static int scan_n = 0;

static void scan_page(int page_slot, int value, int depth)
{
	if (scan_n >= SCAN_MAX || depth > 8 || !cheat_page_valid(page_slot))
		return;
	struct page *p = heap_get_page(page_slot);
	for (int i = 0; i < p->nr_vars && scan_n < SCAN_MAX; i++) {
		enum ain_data_type t = variable_type(p, i, NULL, NULL);
		if (is_intlike(t)) {
			if (p->values[i].i == value) {
				scan_cands[scan_n].page_slot = page_slot;
				scan_cands[scan_n].varno = i;
				scan_n++;
			}
		} else {
			switch (t) {
			case AIN_STRUCT:
			case AIN_ARRAY_TYPE:
				if (p->values[i].i > 0)
					scan_page(p->values[i].i, value, depth + 1);
				break;
			default:
				break;
			}
		}
	}
}

static char *cheat_var_name(int page_slot, int varno)
{
	if (page_slot == 0 && ain && varno < ain->nr_globals)
		return sjis2utf(ain->globals[varno].name, 0);
	char buf[48];
	snprintf(buf, sizeof(buf), "[слот %d] #%d", page_slot, varno);
	return xstrdup(buf);
}

int bridge_cheat_scan(int value, bool narrow, struct bridge_var **out, int max)
{
	*out = NULL;
	if (!scan_cands)
		scan_cands = xcalloc(SCAN_MAX, sizeof(*scan_cands));
	if (!narrow) {
		scan_n = 0;
		scan_page(0, value, 0);
	} else {
		int kept = 0;
		for (int i = 0; i < scan_n; i++) {
			int ok;
			int v = bridge_cheat_read(scan_cands[i].page_slot,
			                          scan_cands[i].varno, &ok);
			if (ok && v == value)
				scan_cands[kept++] = scan_cands[i];
		}
		scan_n = kept;
	}

	int n = scan_n < max ? scan_n : max;
	struct bridge_var *arr = xcalloc(n > 0 ? n : 1, sizeof(struct bridge_var));
	for (int i = 0; i < n; i++) {
		int ok;
		arr[i].page_slot = scan_cands[i].page_slot;
		arr[i].varno = scan_cands[i].varno;
		arr[i].value = bridge_cheat_read(arr[i].page_slot, arr[i].varno, &ok);
		arr[i].name_utf8 = cheat_var_name(arr[i].page_slot, arr[i].varno);
	}
	*out = arr;
	return scan_n;   // полное число кандидатов (в *out — первые max)
}

void bridge_cheat_free(struct bridge_var *arr, int n)
{
	if (!arr)
		return;
	for (int i = 0; i < n; i++)
		free(arr[i].name_utf8);
	free(arr);
}

// Приглушение музыки на время фразы TTS. percent — целевой уровень (0..100)
// относительно исходной громкости; on=false восстанавливает исходную.
#define MAX_DUCKED 8
static struct { int idx; int saved_volume; } ducked[MAX_DUCKED];
static int nr_ducked = 0;

void bridge_duck_music(bool on, int percent)
{
	// Приглушение музыки на время речи — временное, не персистить в настройки.
	mixer_suppress_save(true);
	if (on) {
		if (nr_ducked) {  // уже приглушено
			mixer_suppress_save(false);
			return;
		}
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
	mixer_suppress_save(false);
}

// Строка отображается на экране ровно в момент add_text, поэтому озвучиваем
// сразу здесь (не откладывая до line_break — иначе TTS отстаёт на строку).
void bridge_adv_add_text(struct string *sjis_text)
{
	if (!sjis_text || !sjis_text->size)
		return;
	char *utf8 = sjis2utf(sjis_text->text, sjis_text->size);
	bridge_trace("add_text |%s|", utf8);
	bridge_emit(utf8, line_has_voice);
	free(utf8);
	line_has_voice = false;
}

void bridge_adv_add_voice(int voice_no)
{
	bridge_trace("add_voice %d", voice_no);
	line_has_voice = true;
}

static void bridge_emit_page_break(void);

// Новая страница диалога: сбросить очередь TTS (не дочитывать прошлое).
void bridge_adv_page_break(void)
{
	bridge_trace("PAGE_break");
	line_has_voice = false;
	bridge_emit_page_break();
}

// Перевод строки внутри страницы — текст уже озвучен в add_text, здесь только
// отладочная отметка и (на Linux) разовый тест заглушения.
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
	// XS4_CHEAT_TEST=<число>: разово просканировать глобалы по значению
	static bool cheat_tested = false;
	if (!cheat_tested && getenv("XS4_CHEAT_TEST")) {
		cheat_tested = true;
		struct bridge_var *vars;
		int total = bridge_cheat_scan(atoi(getenv("XS4_CHEAT_TEST")), false, &vars, 10);
		printf("[CHEAT] кандидатов со значением %s: %d\n",
		       getenv("XS4_CHEAT_TEST"), total);
		for (int i = 0; i < (total < 10 ? total : 10); i++)
			printf("[CHEAT]   slot=%d varno=%d %s = %d\n",
			       vars[i].page_slot, vars[i].varno,
			       vars[i].name_utf8, vars[i].value);
		bridge_cheat_free(vars, total < 10 ? total : 10);
		struct bridge_var *lst;
		int ln = bridge_cheat_list(NULL, &lst, 5);
		printf("[CHEAT] первые %d глобалов:\n", ln);
		for (int i = 0; i < ln; i++)
			printf("[CHEAT]   varno=%d %s = %d\n", lst[i].varno,
			       lst[i].name_utf8, lst[i].value);
		bridge_cheat_free(lst, ln);
		fflush(stdout);
	}
#endif
	bridge_trace("line_break");
}

#ifndef __ANDROID__
#include <stdarg.h>
static void bridge_trace(const char *fmt, ...)
{
	if (!getenv("XS4_BRIDGE_DEBUG"))
		return;
	va_list ap;
	va_start(ap, fmt);
	fputs("[TRACE] ", stdout);
	vprintf(fmt, ap);
	putchar('\n');
	va_end(ap);
	fflush(stdout);
}

static void bridge_emit(const char *utf8_text, bool has_voice)
{
	if (!getenv("XS4_BRIDGE_DEBUG"))
		return;
	printf("[ADV] voice=%d |%s|\n", has_voice, utf8_text);
	fflush(stdout);
}

static void bridge_emit_page_break(void)
{
	if (!getenv("XS4_BRIDGE_DEBUG"))
		return;
	printf("[ADV] --- page ---\n");
	fflush(stdout);
}

void bridge_on_skip(int state)
{
	if (getenv("XS4_BRIDGE_DEBUG"))
		printf("[SKIP] state=%d\n", state);
}

#else /* __ANDROID__ */

#include <jni.h>
#include <stdarg.h>
#include <android/log.h>
#define BLOG(...) __android_log_print(ANDROID_LOG_INFO, "xs4bridge", __VA_ARGS__)

static void bridge_trace(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	__android_log_vprint(ANDROID_LOG_INFO, "xs4bridge", fmt, ap);
	va_end(ap);
}

static JavaVM *jvm = NULL;
static jclass bridge_class = NULL;      // GlobalRef на NativeBridge
static jmethodID mid_on_adv_text = NULL;
static jmethodID mid_on_adv_page = NULL;
static jmethodID mid_on_skip = NULL;

/* NativeBridge — Kotlin object: external fun-методы НЕ статические,
 * вторым JNI-аргументом приходит экземпляр синглтона (jobject). */
JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeInit(JNIEnv *env, jobject self)
{
	(*env)->GetJavaVM(env, &jvm);
	jclass cls = (*env)->GetObjectClass(env, self);
	bridge_class = (*env)->NewGlobalRef(env, cls);
	mid_on_adv_text = (*env)->GetStaticMethodID(env, bridge_class, "onAdvText",
	                                            "(Ljava/lang/String;Z)V");
	if (!mid_on_adv_text)
		(*env)->ExceptionClear(env);
	mid_on_adv_page = (*env)->GetStaticMethodID(env, bridge_class, "onAdvPage", "()V");
	if (!mid_on_adv_page)
		(*env)->ExceptionClear(env);
	mid_on_skip = (*env)->GetStaticMethodID(env, bridge_class, "onSkip", "(I)V");
	if (!mid_on_skip)
		(*env)->ExceptionClear(env);
	BLOG("nativeInit: text=%p page=%p", (void*)mid_on_adv_text, (void*)mid_on_adv_page);
}

JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeSetTts(
		JNIEnv *env, jobject self, jboolean on)
{
	(void)env; (void)self;
	bridge_set_tts_enabled(on);
	bridge_set_voice_muted(on);
}

JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeSetLetterSpacing(
		JNIEnv *env, jobject self, jfloat base, jfloat large, jfloat threshold)
{
	(void)env; (void)self;
	gfx_set_letter_spacing(base, large, threshold);
}

JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeSetInfiniteEvents(
		JNIEnv *env, jobject self, jboolean on)
{
	(void)env; (void)self;
	bridge_set_infinite_events(on);
}

JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeDuckMusic(
		JNIEnv *env, jobject self, jboolean on, jint percent)
{
	(void)env; (void)self;
	bridge_duck_music(on, percent);
}

JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeAdvance(
		JNIEnv *env, jobject self)
{
	(void)env; (void)self;
	bridge_advance_message();
}

JNIEXPORT jint JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeUiDrawCount(
		JNIEnv *env, jobject self)
{
	(void)env; (void)self;
	return (jint)bridge_ui_draw_count();
}

JNIEXPORT jint JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeNfCharCount(
		JNIEnv *env, jobject self)
{
	(void)env; (void)self;
	return (jint)bridge_nf_char_count();
}

// Забрать и очистить буфер отрисованного текста (SJIS-байты).
JNIEXPORT jbyteArray JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeTakeDrawnBytes(
		JNIEnv *env, jobject self)
{
	(void)self;
	size_t l = drawn_len;
	jbyteArray arr = (*env)->NewByteArray(env, (jsize)l);
	if (arr && l)
		(*env)->SetByteArrayRegion(env, arr, 0, (jsize)l, (const jbyte *)drawn_buf);
	drawn_len = 0;
	drawn_buf[0] = '\0';
	return arr;
}

// Массив строк "pageSlot\tvarno\tname\tvalue" из bridge_var[]
static jobjectArray vars_to_jarray(JNIEnv *env, struct bridge_var *vars, int n)
{
	jclass str_cls = (*env)->FindClass(env, "java/lang/String");
	jobjectArray arr = (*env)->NewObjectArray(env, n, str_cls, NULL);
	for (int i = 0; i < n; i++) {
		char buf[512];
		snprintf(buf, sizeof(buf), "%d\t%d\t%s\t%d",
		         vars[i].page_slot, vars[i].varno,
		         vars[i].name_utf8 ? vars[i].name_utf8 : "?", vars[i].value);
		jstring s = (*env)->NewStringUTF(env, buf);
		if (!s) { (*env)->ExceptionClear(env); continue; }
		(*env)->SetObjectArrayElement(env, arr, i, s);
		(*env)->DeleteLocalRef(env, s);
	}
	return arr;
}

#define CHEAT_UI_MAX 500

JNIEXPORT jobjectArray JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeCheatList(
		JNIEnv *env, jobject self, jstring jfilter)
{
	(void)self;
	const char *filter = jfilter ? (*env)->GetStringUTFChars(env, jfilter, NULL) : NULL;
	struct bridge_var *vars;
	int n = bridge_cheat_list(filter, &vars, CHEAT_UI_MAX);
	if (filter)
		(*env)->ReleaseStringUTFChars(env, jfilter, filter);
	jobjectArray arr = vars_to_jarray(env, vars, n);
	bridge_cheat_free(vars, n);
	return arr;
}

// Элемент [0] результата — "TOTAL:<полное число кандидатов>", далее строки переменных.
JNIEXPORT jobjectArray JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeCheatScan(
		JNIEnv *env, jobject self, jint value, jboolean narrow)
{
	(void)self;
	struct bridge_var *vars;
	int total = bridge_cheat_scan(value, narrow, &vars, CHEAT_UI_MAX);
	int shown = total < CHEAT_UI_MAX ? total : CHEAT_UI_MAX;

	jclass str_cls = (*env)->FindClass(env, "java/lang/String");
	jobjectArray arr = (*env)->NewObjectArray(env, shown + 1, str_cls, NULL);
	char hdr[32];
	snprintf(hdr, sizeof(hdr), "TOTAL:%d", total);
	jstring h = (*env)->NewStringUTF(env, hdr);
	(*env)->SetObjectArrayElement(env, arr, 0, h);
	(*env)->DeleteLocalRef(env, h);
	for (int i = 0; i < shown; i++) {
		char buf[512];
		snprintf(buf, sizeof(buf), "%d\t%d\t%s\t%d",
		         vars[i].page_slot, vars[i].varno,
		         vars[i].name_utf8 ? vars[i].name_utf8 : "?", vars[i].value);
		jstring s = (*env)->NewStringUTF(env, buf);
		if (!s) { (*env)->ExceptionClear(env); continue; }
		(*env)->SetObjectArrayElement(env, arr, i + 1, s);
		(*env)->DeleteLocalRef(env, s);
	}
	bridge_cheat_free(vars, shown);
	return arr;
}

JNIEXPORT jboolean JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeCheatWrite(
		JNIEnv *env, jobject self, jint pageSlot, jint varno, jint value)
{
	(void)env; (void)self;
	return bridge_cheat_write(pageSlot, varno, value);
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
	// Гейта по tts_enabled нет: текст нужен и истории сообщений при выключенной
	// озвучке; решение «читать или нет» принимает Kotlin (TtsSpeaker).
	if (!mid_on_adv_text)
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

static void bridge_emit_page_break(void)
{
	if (!mid_on_adv_page)
		return;
	JNIEnv *env = bridge_env();
	if (!env)
		return;
	(*env)->CallStaticVoidMethod(env, bridge_class, mid_on_adv_page);
	if ((*env)->ExceptionCheck(env))
		(*env)->ExceptionClear(env);
}

void bridge_on_skip(int state)
{
	if (!mid_on_skip)
		return;
	JNIEnv *env = bridge_env();
	if (!env)
		return;
	(*env)->CallStaticVoidMethod(env, bridge_class, mid_on_skip, (jint)state);
	if ((*env)->ExceptionCheck(env))
		(*env)->ExceptionClear(env);
}

#endif /* __ANDROID__ */
