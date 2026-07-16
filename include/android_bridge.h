/* Мост движок <-> Android (TTS, читы). На не-Android платформах
 * собирается заглушкой с отладочным выводом в stdout (XS4_BRIDGE_DEBUG=1). */
#ifndef SYSTEM4_ANDROID_BRIDGE_H
#define SYSTEM4_ANDROID_BRIDGE_H

#include <stdbool.h>

struct string;

// ADV-текст (вызывается из AnteaterADVEngine, поток VM)
void bridge_adv_add_text(struct string *sjis_text);
void bridge_adv_line_break(void);
void bridge_adv_page_break(void);
void bridge_adv_add_voice(int voice_no);

// Управление (вызывается из JNI / заглушки)
void bridge_set_tts_enabled(bool on);
void bridge_set_voice_muted(bool on);
// Перелистнуть сообщение (синтетический Enter; для авто-листания после TTS)
void bridge_advance_message(void);
// Приглушить музыку до percent% исходной громкости (on) / восстановить (off)
void bridge_duck_music(bool on, int percent);

#endif /* SYSTEM4_ANDROID_BRIDGE_H */
