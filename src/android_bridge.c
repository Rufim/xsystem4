/* Мост движок <-> Android: пересылка ADV-текста для TTS,
 * заглушение голосовых микшеров, доступ к переменным VM для читов.
 *
 * Общая часть платформонезависима; эмиттер текста имеет две реализации:
 * JNI (Android) и stdout-заглушка (прочие платформы, для отладки).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system4.h"
#include "system4/string.h"
#include "system4/utfsjis.h"
#include "android_bridge.h"

static bool tts_enabled = false;
static bool line_has_voice = false;
static struct string *line_buf = NULL;

static void bridge_emit(const char *utf8_text, bool has_voice);

void bridge_set_tts_enabled(bool on)
{
	tts_enabled = on;
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
#endif
